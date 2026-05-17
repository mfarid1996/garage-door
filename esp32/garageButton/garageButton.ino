#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ESP32Servo.h>
#include <esp_task_wdt.h>
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

void onMessage(char* topic, byte* payload, unsigned int len) {
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
      if (mqtt.connect("esp32-garage", MQTT_USER, MQTT_PASS)) {
        Serial.println(" connected");
        mqtt.subscribe(MQTT_TOPIC);
        return;
      }
      Serial.printf(" failed (rc=%d), retry in %lums\n", mqtt.state(), backoff);
      lastAttempt = millis();
      backoff *= 2;
      if (backoff > 5000) backoff = 5000;
    }

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
  if (!mqtt.connected()) {
    digitalWrite(LED_PIN, LOW);
    connectMQTT();
    digitalWrite(LED_PIN, HIGH);
  }
  mqtt.loop();
  esp_task_wdt_reset();
}
