#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ESP32Servo.h>
#include <esp_task_wdt.h>
#include <esp_system.h>   // esp_reset_reason()
#include <Preferences.h>
#include "secrets.h"

// ── Hardware — Arduino Nano ESP32 (u-blox NORA-W106 / ESP32-S3) ───────────────
// These are ARDUINO pin numbers, NOT GPIO numbers. The nano_nora variant builds
// with BOARD_HAS_PIN_REMAP, so a bare `13` would silently resolve to D13/GPIO48
// (which is also SCK and the onboard LED) rather than GPIO13. Always use the
// Dn/An constants here — the remap is applied once, inside the core's pinMode /
// digitalWrite / ledcAttach macros.
//   LED_BUILTIN → D13 → GPIO48, the onboard green LED. Active HIGH, so the
//                 existing HIGH = on logic carries over unchanged. (The separate
//                 RGB LED on this board is active LOW — do not use it here.)
//   D6          → GPIO9, servo signal. A plain GPIO with no strapping or boot
//                 function; D0/D1 are UART0 and spew the boot log at reset,
//                 which would put garbage pulses on a servo line.
//
// Servo wiring: signal → D6, V+ → VBUS, GND → the GND pin below D2.
// VBUS is the only 5 V source on this board and it is live ONLY when powered
// over USB-C. VIN is an input (6–21 V) and will not source 5 V to the servo.
#define LED_PIN   LED_BUILTIN
#define SERVO_PIN D6

Servo servo;
WiFiClientSecure net;
PubSubClient mqtt(net);

// ── Timings ───────────────────────────────────────────────────────────────────
// If a stage exceeds its budget, we ESP.restart() and try again from scratch —
// faster and more predictable than spinning in a half-broken state. The hardware
// watchdog is a backstop for anything that wedges without feeding it.
const unsigned long WIFI_CONNECT_BUDGET_MS = 20000;
const unsigned long MQTT_CONNECT_BUDGET_MS = 15000;
const uint32_t      WDT_TIMEOUT_S          = 45;

// The pinned channel/BSSID below skips the full scan and saves ~1–2 s per connect,
// but it records one moment in time. If the router changes channel or the AP is
// swapped, a pinned begin() can never associate and the board would reboot-loop on
// WIFI_CONNECT_BUDGET_MS forever. So the pin gets a short window, then we fall back
// to an unpinned scan for the rest of the budget.
const unsigned long WIFI_PINNED_ATTEMPT_MS = 8000;

const int LED_BLINK_WIFI_MS = 100;   // fast blink while connecting WiFi
const int LED_BLINK_MQTT_MS = 300;   // slower blink while connecting MQTT

// Servo positions and timings — single-shot moves at full mechanical speed.
const int SERVO_REST_POS    = 45;   // resting / power-on angle (no mechanical preload here)
const int SERVO_TRIGGER_POS = 10;   // angle held while the door opener button is "pressed"
const int SERVO_HOLD_MS     = 150;  // dwell at the trigger position
const int SERVO_MOVE_MS     = 200;  // worst-case time for the servo to physically arrive

// Servo-trigger cooldown — ignore repeat triggers within this window to prevent
// double-actuation when the PWA auto-fires on visibilitychange or the user mashes.
const unsigned long TRIGGER_COOLDOWN_MS = 10000;
unsigned long lastTriggerMs = 0;
bool hasTriggered = false;

// ── Session / downtime reporting ──────────────────────────────────────────────
// Two NVS-persisted counters survive reboots and power cuts, so the dashboard can
// tell "the broker dropped us" apart from "the board rebooted". They are written
// ONLY on the two discrete events below (a few writes a day) — never on a timer.
// A periodic write would burn the NVS flash sector for no extra information.
Preferences prefs;
const char* NVS_NAMESPACE   = "garage";
const char* NVS_KEY_BOOTS   = "boots";    // bumped once per setup()
const char* NVS_KEY_SESSION = "session";  // bumped on every successful MQTT connect

uint32_t bootCount    = 0;
uint32_t sessionCount = 0;

// Why the last reset happened. Reported alongside the counters so a reboot can be
// attributed instead of guessed: a brownout (power/servo), our own ESP.restart()
// after a connect budget overrun ("sw"), a watchdog panic, or a genuine power cut.
const char* resetReason = "unknown";

// esp_reset_reason() reports ESP_RST_SW for BOTH our own ESP.restart() and a host
// re-flash: the core's 1200bps-touch path ends in esp_restart() with no reason hint,
// so the two are indistinguishable from the enum alone. That matters — "the board
// restarted itself because WiFi kept failing" and "Mark just reflashed it" are
// opposite diagnoses. RTC memory survives a software reset but not a power cycle, so
// a magic value written immediately before our own restart separates them.
RTC_NOINIT_ATTR uint32_t selfRestartMagic;
const uint32_t SELF_RESTART_MAGIC = 0x9E7B0075;

void selfRestart(const char* why) {
  Serial.printf("%s — restarting\n", why);
  selfRestartMagic = SELF_RESTART_MAGIC;
  Serial.flush();
  ESP.restart();
}

const char* resetReasonName() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "poweron";   // cold start / power restored
    case ESP_RST_EXT:       return "ext";       // reset pin
    case ESP_RST_SW:        return selfRestartMagic == SELF_RESTART_MAGIC
                                   ? "budget"     // our own restart, connect budget blown
                                   : "hostreset"; // 1200bps touch — a re-flash
    case ESP_RST_PANIC:     return "panic";     // crash, incl. the task-WDT panic handler
    case ESP_RST_INT_WDT:   return "intwdt";
    case ESP_RST_TASK_WDT:  return "taskwdt";
    case ESP_RST_WDT:       return "wdt";
    case ESP_RST_BROWNOUT:  return "brownout";  // 3.3 V rail sagged — suspect servo on VBUS
    case ESP_RST_PWR_GLITCH:return "pwrglitch"; // ditto, but a fast transient rather than a sag
    case ESP_RST_CPU_LOCKUP:return "lockup";    // double exception
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_SDIO:      return "sdio";
    case ESP_RST_USB:       return "usb";       // host reset the USB-Serial/JTAG peripheral —
                                                // i.e. a flash or an esptool reset, not a fault
    case ESP_RST_JTAG:      return "jtag";
    case ESP_RST_EFUSE:     return "efuse";
    default:                return "unknown";
  }
}

// millis() keeps running through a WiFi/MQTT drop as long as the board does not
// reboot, so the outage can be measured exactly by timestamping the moment the
// loss is observed. After a boot millis() has reset and the device genuinely
// cannot know how long it was dead — that case reports -1 instead of guessing.
const long DOWN_MS_UNKNOWN = -1;

bool          gapOpen      = false;  // an outage is in progress, awaiting its report
unsigned long gapStartMs   = 0;      // millis() when the loss was first observed
bool          gapLostWiFi  = false;  // the radio dropped too, not just the MQTT session
bool          firstConnect = true;   // the connect that follows setup() is a "boot"

// Attach only for the duration of a move, then release. The servo is powered from
// VBUS with no bulk capacitor, so the less time it draws current the better: an
// attached SG90 holds position with a live pulse train and hunts slightly forever,
// while a detached one is limp and draws ~nothing.
//
// SAFETY PRECONDITION: this is only sound because there is no mechanical preload at
// the rest angle — Mark verified that on the bench 2026-08-21, superseding the older
// "pre-loaded toward trigger direction" note. A detached SG90 is held by gear
// friction alone, and drift is one-sided in consequence: away from the button is
// harmless, toward it opens a garage. If the linkage is ever re-mounted or given a
// spring, revert to a permanently-attached servo before running it on a live door.
//
// Re-attaching does not twitch the arm, but NOT because the duty starts at zero:
// ledcAttachChannel() reads the channel's existing duty back out of the peripheral
// and restores it (esp32-hal-ledc.c), so a re-attach resumes the rest angle that was
// last written. On a cold boot the duty genuinely is 0, which is what makes
// servoPark() safe. Do not "simplify" this on the assumption that attach() is
// always silent — after the first actuation it is not.
//
// To go back to a permanently-attached servo, drop the attach/detach pair here and
// restore a plain servo.attach() in setup().
//
// Callers must confirm the attach took: writeTicks() silently no-ops while detached,
// which would otherwise produce a cheerful ack for a door that never moved.
//
// Check attached(), NOT the return value of attach(). attach() returns the allocated
// LEDC channel number and 0 on failure — and 0 is also a perfectly valid channel, so
// `if (!servo.attach(...))` rejects a successful attach on channel 0. That is not
// hypothetical: it is the first channel handed out, so it fires on every boot.
bool servoActuate() {
  servo.attach(SERVO_PIN);
  if (!servo.attached()) {
    Serial.println("servo.attach FAILED — not actuating");
    return false;
  }
  servo.write(SERVO_TRIGGER_POS);
  delay(SERVO_MOVE_MS);   // let it physically arrive
  delay(SERVO_HOLD_MS);   // dwell on the button
  servo.write(SERVO_REST_POS);
  delay(SERVO_MOVE_MS);
  servo.detach();
  return true;
}

// Park at the rest angle once at boot, then release.
void servoPark() {
  servo.attach(SERVO_PIN);
  if (!servo.attached()) {
    Serial.println("servo.attach FAILED at boot — check SERVO_PIN");
    return;
  }
  servo.write(SERVO_REST_POS);
  delay(SERVO_MOVE_MS);
  servo.detach();
}

// Read-increment-write in one short open/close so a brownout mid-update cannot
// leave the namespace held open.
uint32_t bumpCounter(const char* key) {
  prefs.begin(NVS_NAMESPACE, false);
  uint32_t value = prefs.getUInt(key, 0) + 1;
  prefs.putUInt(key, value);
  prefs.end();
  return value;
}

// Called from both the WiFi and the MQTT watchpoints. The first observation wins:
// the radio usually drops before PubSubClient notices, and that earlier timestamp
// is the true start of the outage.
void noteDisconnect(bool wifiLost) {
  if (!gapOpen) {
    gapOpen    = true;
    gapStartMs = millis();
  }
  if (wifiLost) gapLostWiFi = true;
}

// Retained so a browser that loads mid-session still sees the last reconnect.
// Hand-rolled with snprintf rather than pulling in ArduinoJson for five fields.
// `rst` is additive: poll-status.mjs picks named fields off the parsed object and
// ignores the rest, so an older deployed poller keeps working untouched.
void publishSession(const char* reason, long downMs) {
  char json[160];
  snprintf(json, sizeof(json),
           "{\"session\":%lu,\"boots\":%lu,\"downMs\":%ld,\"reason\":\"%s\",\"rst\":\"%s\"}",
           (unsigned long)sessionCount, (unsigned long)bootCount, downMs, reason, resetReason);
  mqtt.publish("garage/session", json, true);
  Serial.printf("Session published: %s\n", json);
}

void onMessage(char* topic, byte* payload, unsigned int len) {
  if (hasTriggered && millis() - lastTriggerMs < TRIGGER_COOLDOWN_MS) {
    unsigned long remainingMs = TRIGGER_COOLDOWN_MS - (millis() - lastTriggerMs);
    unsigned long remainingS  = (remainingMs + 999) / 1000;
    char ackMsg[64];
    snprintf(ackMsg, sizeof(ackMsg), "cooldown - ignored, %lus left", remainingS);
    mqtt.publish("garage/ack", ackMsg);
    Serial.printf("Trigger ignored — %lus cooldown remaining\n", remainingS);
    return;
  }
  hasTriggered = true;
  lastTriggerMs = millis();

  char ackMsg[64];
  snprintf(ackMsg, sizeof(ackMsg), "trigger received, uptime %lus", millis() / 1000);
  mqtt.publish("garage/ack", ackMsg);  // immediate ack on receive

  if (!servoActuate()) {
    // The ack above already claimed receipt; correct the record so the PWA is not
    // told "Garage confirmed" for a door that never moved.
    mqtt.publish("garage/ack", "ERROR: servo attach failed - not actuated");
  }

  // 3 quick flicker-offs against the solid-on baseline, ending back at solid on.
  for (int i = 0; i < 3; i++) {
    digitalWrite(LED_PIN, LOW);  delay(120);
    digitalWrite(LED_PIN, HIGH); delay(120);
  }

  Serial.println("Trigger received — servo actuated, LED blinked");
}

void connectWiFi() {
  bool pinned = false;
#ifdef WIFI_CHANNEL
  WiFi.begin(WIFI_SSID, WIFI_PASS, WIFI_CHANNEL, WIFI_BSSID);
  pinned = true;
#else
  WiFi.begin(WIFI_SSID, WIFI_PASS);
#endif
  WiFi.setSleep(false);  // lower latency, marginal extra current draw

  Serial.print("Connecting to WiFi");
  const unsigned long start = millis();
  unsigned long lastBlink = 0;
  bool ledOn = false;

  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > WIFI_CONNECT_BUDGET_MS) {
      selfRestart("\nWiFi connect budget exceeded");
    }
    // A stale pin can never associate, so it must not be allowed to consume the
    // whole budget — otherwise a router channel change bricks the board into a
    // reboot loop. Give up on it early and let the remaining budget cover a scan.
    if (pinned && millis() - start > WIFI_PINNED_ATTEMPT_MS) {
      Serial.println("\nPinned BSSID/channel did not associate — falling back to a full scan");
      pinned = false;
      WiFi.disconnect();
      // Report a refused begin() rather than silently burning the rest of the budget
      // and rebooting with no clue why the fallback never worked.
      if (WiFi.begin(WIFI_SSID, WIFI_PASS) == WL_CONNECT_FAILED) {
        Serial.println("WiFi.begin() refused the unpinned retry");
      }
    }
    unsigned long now = millis();
    if (now - lastBlink >= LED_BLINK_WIFI_MS) {
      ledOn = !ledOn;
      digitalWrite(LED_PIN, ledOn ? HIGH : LOW);
      lastBlink = now;
      Serial.print(".");
    }
    esp_task_wdt_reset();
    delay(10);
  }

  Serial.printf("\nWiFi connected: %s  BSSID %s  ch %d  RSSI %d\n",
                WiFi.localIP().toString().c_str(),
                WiFi.BSSIDstr().c_str(),
                WiFi.channel(),
                WiFi.RSSI());
}

void connectMQTT() {
  const unsigned long start = millis();
  unsigned long lastBlink = 0;
  unsigned long lastAttempt = 0;
  unsigned long backoff = 500;
  bool ledOn = false;
  bool firstAttempt = true;

  while (!mqtt.connected()) {
    if (millis() - start > MQTT_CONNECT_BUDGET_MS) {
      selfRestart("MQTT connect budget exceeded");
    }

    if (firstAttempt || millis() - lastAttempt >= backoff) {
      firstAttempt = false;
      Serial.print("Connecting to MQTT...");
      // cleanSession=false: broker holds our subscription + queues triggers while we're offline.
      // LWT publishes "offline" (retained) to garage/status if our connection drops ungracefully.
      if (mqtt.connect("esp32-garage",
                       MQTT_USER, MQTT_PASS,
                       "garage/status", 1, true, "offline",
                       false)) {
        Serial.println(" connected");
        mqtt.publish("garage/status", "online", true);  // overwrite the will while we're up

        sessionCount = bumpCounter(NVS_KEY_SESSION);
        const char* reason = firstConnect ? "boot" : (gapLostWiFi ? "wifi" : "mqtt");
        long downMs = firstConnect ? DOWN_MS_UNKNOWN : (long)(millis() - gapStartMs);
        publishSession(reason, downMs);
        firstConnect = false;
        gapOpen      = false;
        gapLostWiFi  = false;

        mqtt.subscribe(MQTT_TOPIC);
        return;
      }
      Serial.printf(" failed (rc=%d), retry in %lums\n", mqtt.state(), backoff);
      lastAttempt = millis();
      backoff *= 2;
      if (backoff > 5000) backoff = 5000;
    }

    // Polled every pass, not just per attempt: the radio can drop mid-backoff, and
    // that is what makes this outage a "wifi" one rather than a broker-side "mqtt" one.
    if (WiFi.status() != WL_CONNECTED) noteDisconnect(true);

    unsigned long now = millis();
    if (now - lastBlink >= LED_BLINK_MQTT_MS) {
      ledOn = !ledOn;
      digitalWrite(LED_PIN, ledOn ? HIGH : LOW);
      lastBlink = now;
    }
    esp_task_wdt_reset();
    delay(10);
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  servoPark();

  // Latched once: esp_reset_reason() is stable for the life of the boot, and the
  // session payload wants it long after setup() has returned. ESP_RST_BROWNOUT is
  // the one that matters here — it means the 3.3 V rail sagged, which on this
  // board points at the servo sharing VBUS rather than at anything in software.
  resetReason = resetReasonName();
  // Consume the marker: whatever caused the NEXT boot must prove itself again.
  selfRestartMagic = 0;

  // One NVS write per power-on / reset. Done before the network stages so the boot
  // is still counted if WiFi or MQTT blows its budget and forces a restart.
  bootCount = bumpCounter(NVS_KEY_BOOTS);
  Serial.printf("\nBoot #%lu (reset reason: %s)\n", (unsigned long)bootCount, resetReason);

  // Reconfigure the auto-initialised watchdog with our timeout and add the loop task.
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms     = WDT_TIMEOUT_S * 1000,
    .idle_core_mask = 0,
    .trigger_panic  = true,
  };
  esp_task_wdt_reconfigure(&wdt_config);
  esp_task_wdt_add(NULL);

  connectWiFi();

  net.setInsecure();  // skip cert verification — fine for hobby use
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMessage);
  connectMQTT();

  digitalWrite(LED_PIN, HIGH);  // solid = ready to receive commands
  Serial.println("Ready — waiting for trigger");
}

void loop() {
  // Watched separately from MQTT because the radio normally drops first; catching it
  // here timestamps the real start of the outage instead of PubSubClient's later notice.
  if (WiFi.status() != WL_CONNECTED) noteDisconnect(true);

  if (!mqtt.connected()) {
    noteDisconnect(false);
    digitalWrite(LED_PIN, LOW);
    connectMQTT();
    digitalWrite(LED_PIN, HIGH);
  }
  mqtt.loop();
  esp_task_wdt_reset();
}
