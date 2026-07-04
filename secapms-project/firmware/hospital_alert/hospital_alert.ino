/*
  =====================================================================
  SECAPMS — Hospital Alert Panel Firmware
  Device kind: hospital_alert  (table: devices, hospitals)
  Board: ESP32 DevKit v1
  Connectivity: Wi-Fi only (no SIM/GSM)
  =====================================================================

  What this device does
  ----------------------
  - Sits at the hospital's ER reception / triage desk.
  - Subscribes to an "alert" topic. When admin approves an emergency
    routed to this hospital, the cloud publishes a JSON payload with
    patient category + ETA context; the panel:
      * sounds the buzzer
      * shows the alert on a 16x2 I2C LCD (or swap for an OLED/TFT)
      * lights a category-colored LED (red=critical, amber=serious,
        green=stable)
  - Staff press the physical READY button once a bed is prepared; this
    publishes an "ack" message back (mirrors the hospital dashboard's
    READY button, useful when staff are at the desk, not a screen).
  - Publishes a heartbeat/LWT so Maintenance/Devices dashboards can see
    the panel is online.

  Hardware (Bill of Materials)
  -----------------------------
  1x ESP32 DevKit v1
  1x 16x2 I2C LCD (address usually 0x27 or 0x3F)
  1x Piezo buzzer (active)
  3x LEDs (red/amber/green) + 220ohm resistors
  1x Push button ("Bed READY") + 10k pulldown or use INPUT_PULLUP

  Wiring
  -------
  LCD SDA   -> GPIO 21
  LCD SCL   -> GPIO 22
  LCD VCC   -> 5V   LCD GND -> GND
  Buzzer    -> GPIO 25
  LED Red   -> GPIO 26   LED Amber -> GPIO 27   LED Green -> GPIO 14
  READY btn -> GPIO 4 (other leg to GND, uses internal pullup)

  Libraries required
  --------------------
  - "PubSubClient" by Nick O'Leary
  - "ArduinoJson" by Benoit Blanchon
  - "LiquidCrystal_I2C" (e.g. by Frank de Brabander / Marco Schwartz fork)

  See ../README.md for build + flashing + broker setup instructions.
  =====================================================================
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ArduinoOTA.h>
#include "secapms_config.h"

#define PIN_BUZZER      25
#define PIN_LED_RED     26
#define PIN_LED_AMBER   27
#define PIN_LED_GREEN   14
#define PIN_BTN_READY   4

LiquidCrystal_I2C lcd(0x27, 16, 2); // change address if your module uses 0x3F

#if MQTT_USE_TLS
WiFiClientSecure netClient;
#else
WiFiClient netClient;
#endif
PubSubClient mqtt(netClient);

String T_ALERT_SUB, T_ACK_PUB, T_LWT;

bool alertActive = false;
String activeEventId = "";
String activeCategory = "";
bool lastButtonState = HIGH;
unsigned long lastWifiAttempt = 0;
unsigned long lastMqttAttempt = 0;
bool otaStarted = false;

// Lets you push firmware fixes to the desk panel over Wi-Fi instead of
// pulling it apart to re-flash over USB.
void setupOTA() {
  ArduinoOTA.setHostname((String("secapms-ha-") + DEVICE_ID).c_str());
  ArduinoOTA.onStart([]() { Serial.println("[OTA] Update starting"); });
  ArduinoOTA.onEnd([]() { Serial.println("[OTA] Update complete"); });
  ArduinoOTA.onError([](ota_error_t error) { Serial.printf("[OTA] Error %u\n", error); });
  ArduinoOTA.begin();
  otaStarted = true;
  Serial.println("[OTA] Ready");
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_LED_RED, OUTPUT);
  pinMode(PIN_LED_AMBER, OUTPUT);
  pinMode(PIN_LED_GREEN, OUTPUT);
  pinMode(PIN_BTN_READY, INPUT_PULLUP);
  digitalWrite(PIN_BUZZER, LOW);

  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();
  showIdleScreen();

  T_ALERT_SUB = String(TOPIC_PREFIX) + "/hospital/" + DEVICE_ID + "/alert";
  T_ACK_PUB   = String(TOPIC_PREFIX) + "/hospital/" + DEVICE_ID + "/ack";
  T_LWT       = String(TOPIC_PREFIX) + "/hospital/" + DEVICE_ID + "/lwt";

  connectWifi();
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqttMessage);
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastWifiAttempt > WIFI_RETRY_MS) { connectWifi(); lastWifiAttempt = millis(); }
  } else if (!otaStarted) {
    setupOTA();
  }
  if (WiFi.status() == WL_CONNECTED && !mqtt.connected()) {
    if (millis() - lastMqttAttempt > MQTT_RETRY_MS) { connectMqtt(); lastMqttAttempt = millis(); }
  }
  if (otaStarted) ArduinoOTA.handle();
  mqtt.loop();
  handleReadyButton();
}

void connectWifi() {
  Serial.println("[WiFi] Connecting...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void connectMqtt() {
  Serial.println("[MQTT] Connecting...");
  String clientId = String("secapms-ha-") + DEVICE_ID;
  bool ok;
  if (strlen(MQTT_USERNAME) > 0) {
    ok = mqtt.connect(clientId.c_str(), MQTT_USERNAME, MQTT_PASSWORD,
                       T_LWT.c_str(), 1, true, "offline");
  } else {
    ok = mqtt.connect(clientId.c_str(), T_LWT.c_str(), 1, true, "offline");
  }
  if (ok) {
    Serial.println("[MQTT] Connected");
    mqtt.publish(T_LWT.c_str(), "online", true);
    mqtt.subscribe(T_ALERT_SUB.c_str());
  } else {
    Serial.print("[MQTT] Failed, rc="); Serial.println(mqtt.state());
  }
}

void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, payload, length)) return;

  activeEventId = String((const char*)(doc["event_id"] | ""));
  activeCategory = String((const char*)(doc["patient_category"] | "critical"));
  alertActive = true;

  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("INCOMING: ");
  lcd.print(activeCategory);
  lcd.setCursor(0, 1); lcd.print("Press READY when set");

  setCategoryLed(activeCategory);
  for (int i = 0; i < 3; i++) { tone(PIN_BUZZER, 2200, 150); delay(250); }
}

void setCategoryLed(const String& cat) {
  digitalWrite(PIN_LED_RED,  cat == "critical");
  digitalWrite(PIN_LED_AMBER, cat == "serious");
  digitalWrite(PIN_LED_GREEN, cat == "stable");
}

void handleReadyButton() {
  bool reading = digitalRead(PIN_BTN_READY);
  if (lastButtonState == HIGH && reading == LOW) {
    delay(30);
    if (digitalRead(PIN_BTN_READY) == LOW && alertActive) {
      publishAck();
      alertActive = false;
      digitalWrite(PIN_LED_RED, LOW); digitalWrite(PIN_LED_AMBER, LOW); digitalWrite(PIN_LED_GREEN, LOW);
      showIdleScreen();
    }
  }
  lastButtonState = reading;
}

void publishAck() {
  if (!mqtt.connected()) return;
  StaticJsonDocument<192> doc;
  doc["device_id"] = DEVICE_ID;
  doc["event_id"] = activeEventId;
  doc["ready"] = true;
  char buf[192];
  size_t n = serializeJson(doc, buf);
  mqtt.publish(T_ACK_PUB.c_str(), buf, n);
  Serial.println("[HOSPITAL] Bed ready acknowledged");
}

void showIdleScreen() {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("SECAPMS");
  lcd.setCursor(0, 1); lcd.print("No incoming alert");
}
