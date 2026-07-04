/*
  =====================================================================
  SECAPMS — Junction Controller Firmware
  Device kind: junction_controller  (table: devices, junctions)
  Board: ESP32 DevKit v1
  Connectivity: Wi-Fi only (no SIM/GSM)
  =====================================================================

  What this device does
  ----------------------
  - Sits at a traffic junction alongside the existing signal controller.
  - Normally lets the junction run its regular signal cycle (relays not
    energized, existing controller box in charge).
  - Subscribes to a "command" topic. When the corridor is approved for
    an ambulance route that includes this junction, the cloud publishes
    "priority_green" -> the ESP32 energizes a relay that forces the
    green phase for the ambulance's approach direction (wired into the
    existing signal controller's external "priority override" input, or,
    on simpler installs, directly drives an auxiliary LED head).
  - Sends a heartbeat every HEARTBEAT_INTERVAL_MS so Maintenance/Devices
    dashboards know the controller is alive (feeds `device_health`).
  - Publishes a retained LWT so a dropped Wi-Fi link shows "offline"
    immediately instead of going silently stale.

  IMPORTANT SAFETY NOTE
  -----------------------
  This firmware is a reference implementation for prototyping and pilots.
  Any deployment that overrides real traffic signals must go through your
  local traffic authority's electrical and safety approval process, use a
  certified relay/interposing panel (not raw GPIO into mains-side
  equipment), and retain a physical manual-override at the controller
  cabinet. Never wire the ESP32 directly to line voltage.

  Hardware (Bill of Materials)
  -----------------------------
  1x ESP32 DevKit v1
  1x 2-channel relay module (5V, opto-isolated) -- one relay per
     "priority" input line on the existing signal controller
  1x Status LED (green) -- local visual confirmation of priority state
  1x 5V supply from the junction cabinet's existing low-voltage rail
     (never tap raw mains into the ESP32)

  Wiring
  -------
  Relay IN1        -> GPIO 26   (drives "force green" input on signal box)
  Relay IN2        -> GPIO 27   (drives "hold red on cross street" input, if used)
  Status LED       -> GPIO 14
  Relay VCC/GND    -> 5V / GND from cabinet low-voltage supply
  ESP32 GND        -> common GND with relay module and signal box logic

  Libraries required
  --------------------
  - "PubSubClient" by Nick O'Leary
  - "ArduinoJson" by Benoit Blanchon

  See ../README.md for build + flashing + broker setup instructions.
  =====================================================================
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include "secapms_config.h"

#define PIN_RELAY_FORCE_GREEN   26
#define PIN_RELAY_HOLD_CROSS    27
#define PIN_STATUS_LED          14
#define RELAY_ACTIVE_LOW        1   // most cheap relay boards trigger LOW; set 0 if yours is active-HIGH

#if MQTT_USE_TLS
WiFiClientSecure netClient;
#else
WiFiClient netClient;
#endif
PubSubClient mqtt(netClient);

String T_COMMAND_SUB, T_HEALTH_PUB, T_LWT;

bool priorityActive = false;
unsigned long lastHeartbeat = 0;
unsigned long lastWifiAttempt = 0;
unsigned long lastMqttAttempt = 0;
unsigned long bootTime = 0;
bool otaStarted = false;

// Lets you push firmware fixes to a roadside unit over Wi-Fi instead of
// pulling the cabinet box to re-flash over USB.
void setupOTA() {
  ArduinoOTA.setHostname((String("secapms-jc-") + DEVICE_ID).c_str());
  ArduinoOTA.onStart([]() { Serial.println("[OTA] Update starting"); });
  ArduinoOTA.onEnd([]() { Serial.println("[OTA] Update complete"); });
  ArduinoOTA.onError([](ota_error_t error) { Serial.printf("[OTA] Error %u\n", error); });
  ArduinoOTA.begin();
  otaStarted = true;
  Serial.println("[OTA] Ready");
}

void relayWrite(uint8_t pin, bool on) {
  bool level = RELAY_ACTIVE_LOW ? !on : on;
  digitalWrite(pin, level ? HIGH : LOW);
}

void setup() {
  Serial.begin(115200);
  bootTime = millis();

  pinMode(PIN_RELAY_FORCE_GREEN, OUTPUT);
  pinMode(PIN_RELAY_HOLD_CROSS, OUTPUT);
  pinMode(PIN_STATUS_LED, OUTPUT);
  relayWrite(PIN_RELAY_FORCE_GREEN, false);
  relayWrite(PIN_RELAY_HOLD_CROSS, false);
  digitalWrite(PIN_STATUS_LED, LOW);

  T_COMMAND_SUB = String(TOPIC_PREFIX) + "/junction/" + DEVICE_ID + "/command";
  T_HEALTH_PUB  = String(TOPIC_PREFIX) + "/junction/" + DEVICE_ID + "/health";
  T_LWT         = String(TOPIC_PREFIX) + "/junction/" + DEVICE_ID + "/lwt";

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

  if (millis() - lastHeartbeat > HEARTBEAT_INTERVAL_MS) {
    publishHeartbeat();
    lastHeartbeat = millis();
  }

  digitalWrite(PIN_STATUS_LED, priorityActive ? HIGH : (millis() / 1000) % 2); // slow blink = idle, solid = priority
}

void connectWifi() {
  Serial.println("[WiFi] Connecting...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void connectMqtt() {
  Serial.println("[MQTT] Connecting...");
  String clientId = String("secapms-jc-") + DEVICE_ID;
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
    mqtt.subscribe(T_COMMAND_SUB.c_str());
  } else {
    Serial.print("[MQTT] Failed, rc="); Serial.println(mqtt.state());
  }
}

void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  StaticJsonDocument<192> doc;
  if (deserializeJson(doc, payload, length)) return;
  String cmd = doc["command"] | "";
  if (cmd == "priority_green") {
    priorityActive = true;
    relayWrite(PIN_RELAY_FORCE_GREEN, true);
    relayWrite(PIN_RELAY_HOLD_CROSS, true);
    Serial.println("[JUNCTION] Priority green ENGAGED");
  } else if (cmd == "normal" || cmd == "override_clear") {
    priorityActive = false;
    relayWrite(PIN_RELAY_FORCE_GREEN, false);
    relayWrite(PIN_RELAY_HOLD_CROSS, false);
    Serial.println("[JUNCTION] Back to normal cycle");
  }
}

void publishHeartbeat() {
  if (!mqtt.connected()) return;
  StaticJsonDocument<192> doc;
  doc["device_id"] = DEVICE_ID;
  doc["uptime_s"] = (millis() - bootTime) / 1000;
  doc["rssi"] = WiFi.RSSI();
  doc["priority_active"] = priorityActive;
  char buf[192];
  size_t n = serializeJson(doc, buf);
  mqtt.publish(T_HEALTH_PUB.c_str(), buf, n);
}
