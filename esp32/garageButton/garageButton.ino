#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ESP32Servo.h>
#include <esp_task_wdt.h>
#include <Preferences.h>
#include "secrets.h"

// ── Hardware ──────────────────────────────────────────────────────────────────
// Blue LED on GPIO2. Red LED is hardwired to 3.3V (power indicator) — not GPIO-controllable.
#define LED_PIN   2
#define SERVO_PIN 13

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

const int LED_BLINK_WIFI_MS = 100;   // fast blink while connecting WiFi
const int LED_BLINK_MQTT_MS = 300;   // slower blink while connecting MQTT

// Servo positions and timings — single-shot moves at full mechanical speed.
const int SERVO_REST_POS    = 45;   // resting / power-on angle (pre-loaded toward trigger direction)
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

// millis() keeps running through a WiFi/MQTT drop as long as the board does not
// reboot, so the outage can be measured exactly by timestamping the moment the
// loss is observed. After a boot millis() has reset and the device genuinely
// cannot know how long it was dead — that case reports -1 instead of guessing.
const long DOWN_MS_UNKNOWN = -1;

bool          gapOpen      = false;  // an outage is in progress, awaiting its report
unsigned long gapStartMs   = 0;      // millis() when the loss was first observed
bool          gapLostWiFi  = false;  // the radio dropped too, not just the MQTT session
bool          firstConnect = true;   // the connect that follows setup() is a "boot"

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
// Hand-rolled with snprintf rather than pulling in ArduinoJson for four fields.
void publishSession(const char* reason, long downMs) {
  char json[96];
  snprintf(json, sizeof(json),
           "{\"session\":%lu,\"boots\":%lu,\"downMs\":%ld,\"reason\":\"%s\"}",
           (unsigned long)sessionCount, (unsigned long)bootCount, downMs, reason);
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

  servo.write(SERVO_TRIGGER_POS);
  delay(SERVO_MOVE_MS);
  delay(SERVO_HOLD_MS);
  servo.write(SERVO_REST_POS);
  delay(SERVO_MOVE_MS);

  // 3 quick flicker-offs against the solid-on baseline, ending back at solid on.
  for (int i = 0; i < 3; i++) {
    digitalWrite(LED_PIN, LOW);  delay(120);
    digitalWrite(LED_PIN, HIGH); delay(120);
  }

  Serial.println("Trigger received — servo actuated, LED blinked");
}

void connectWiFi() {
#ifdef WIFI_CHANNEL
  WiFi.begin(WIFI_SSID, WIFI_PASS, WIFI_CHANNEL, WIFI_BSSID);
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
      Serial.println("\nWiFi connect budget exceeded — restarting");
      ESP.restart();
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
      Serial.println("MQTT connect budget exceeded — restarting");
      ESP.restart();
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

  servo.attach(SERVO_PIN);
  servo.write(SERVO_REST_POS);

  // One NVS write per power-on / reset. Done before the network stages so the boot
  // is still counted if WiFi or MQTT blows its budget and forces a restart.
  bootCount = bumpCounter(NVS_KEY_BOOTS);
  Serial.printf("\nBoot #%lu\n", (unsigned long)bootCount);

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
