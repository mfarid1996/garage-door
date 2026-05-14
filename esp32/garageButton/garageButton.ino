#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ESP32Servo.h>
#include "secrets.h"

// ── Hardware ──────────────────────────────────────────────────────────────────
#define LED_PIN   2
#define SERVO_PIN 13

Servo servo;

WiFiClientSecure net;
PubSubClient mqtt(net);

// Single-shot move: lets the servo run at its full mechanical speed.
// SERVO_MOVE_MS is the worst-case time for it to physically arrive.
const int SERVO_MOVE_MS = 200;

void onMessage(char* topic, byte* payload, unsigned int len) {
  char ackMsg[64];
  snprintf(ackMsg, sizeof(ackMsg), "trigger received, uptime %lus", millis() / 1000);
  mqtt.publish("garage/ack", ackMsg);  // immediate ack on receive

  servo.write(15);
  delay(SERVO_MOVE_MS);
  delay(1000);
  servo.write(90);
  delay(SERVO_MOVE_MS);

  // Blink 3 times to confirm trigger handled
  for (int i = 0; i < 3; i++) {
    digitalWrite(LED_PIN, HIGH); delay(150);
    digitalWrite(LED_PIN, LOW);  delay(150);
  }

  Serial.println("Trigger received — servo actuated, LED blinked");
}

void connectMQTT() {
  while (!mqtt.connected()) {
    Serial.print("Connecting to MQTT...");
    if (mqtt.connect("esp32-garage", MQTT_USER, MQTT_PASS)) {
      Serial.println(" connected");
      mqtt.subscribe(MQTT_TOPIC);
    } else {
      Serial.printf(" failed (rc=%d), retrying in 5s\n", mqtt.state());
      delay(5000);
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(3000);
  pinMode(LED_PIN, OUTPUT);

  servo.attach(SERVO_PIN);
  servo.write(90);  // neutral center position

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\nWiFi connected: " + WiFi.localIP().toString());

  net.setInsecure();  // skip cert verification — fine for hobby use
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMessage);
  connectMQTT();
  Serial.println("Ready — waiting for trigger");
}

void loop() {
  if (!mqtt.connected()) connectMQTT();
  mqtt.loop();
}
