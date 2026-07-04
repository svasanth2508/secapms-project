/*
  =====================================================================
  SECAPMS — Ambulance Unit Firmware
  Device kind: ambulance_unit  (table: devices, ambulances, gps_logs)
  Board: ESP32 DevKit v1 (or any ESP32-WROOM board)
  Connectivity: Wi-Fi only (no SIM/GSM)
  =====================================================================

  What this device does
  ----------------------
  - Reads live GPS position from a NEO-6M module (UART).
  - Reads battery voltage through a resistor-divider on an ADC pin.
  - Driver presses the big physical ACTIVATE button -> publishes an
    "event" MQTT message (mirrors the driver app's ACTIVATE EMERGENCY
    button, so the corridor can be triggered from the vehicle even if
    the driver's phone is unavailable).
  - Publishes GPS telemetry every TELEMETRY_INTERVAL_MS so the admin
    map and public portal can show the ambulance moving live.
  - Subscribes to its own /status topic so the onboard LED + buzzer
    reflect admin approval (blink amber = pending, solid green =
    approved/in progress, off = completed).
  - Publishes a retained "online"/"offline" LWT so Devices/Maintenance
    dashboards know instantly if a unit drops off Wi-Fi.

  Hardware (Bill of Materials)
  -----------------------------
  1x ESP32 DevKit v1 (30-pin)
  1x NEO-6M GPS module (UART, 3.3V logic)
  1x Push button (panel-mounted, normally-open) - the ACTIVATE button
  1x Piezo buzzer (active, 3.3V/5V)
  1x RGB LED or 3x discrete LEDs (red/amber/green) + 220ohm resistors
  1x Voltage divider (2x 10k resistors) from battery+ to GND, tap to ADC
  1x 5V -> 3.3V buck or use the board's onboard regulator from a 5V supply
  Optional: SIM/GSM module -- NOT USED. Wi-Fi hotspot from vehicle router
            or phone hotspot is the only connectivity path in this design.

  Wiring
  -------
  NEO-6M   TX  -> ESP32 GPIO16 (RX2)
  NEO-6M   RX  -> ESP32 GPIO17 (TX2)   [optional, only if you configure the module]
  NEO-6M   VCC -> 3.3V
  NEO-6M   GND -> GND

  ACTIVATE button -> GPIO 4  (other leg to GND, uses internal pullup)
  Buzzer (+)      -> GPIO 25 (through NPN transistor if buzzer draws >40mA)
  LED Red         -> GPIO 26
  LED Amber       -> GPIO 27
  LED Green       -> GPIO 14
  Battery sense   -> GPIO 34 (ADC1_CH6, via divider, max 3.3V at pin)

  Libraries required (Arduino IDE > Tools > Manage Libraries)
  -------------------------------------------------------------
  - "PubSubClient" by Nick O'Leary
  - "TinyGPSPlus" by Mikal Hart
  - "ArduinoJson" by Benoit Blanchon (v6 or v7)
  - ESP32 board package installed via Boards Manager
    (Board URL: https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json)

  See ../README.md for full build + flashing + broker setup instructions.
  =====================================================================
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <TinyGPSPlus.h>
#include <ArduinoJson.h>
#include <HardwareSerial.h>
#include <ArduinoOTA.h>
#include "secapms_config.h"

// ---------- Pins ----------
#define PIN_BTN_ACTIVATE   4
#define PIN_BUZZER         25
#define PIN_LED_RED        26
#define PIN_LED_AMBER      27
#define PIN_LED_GREEN      14
#define PIN_BATTERY_ADC    34
#define GPS_RX_PIN         16   // ESP32 RX2 <- GPS TX
#define GPS_TX_PIN         17   // ESP32 TX2 -> GPS RX (optional)

HardwareSerial GPSSerial(2);
TinyGPSPlus gps;

#if MQTT_USE_TLS
WiFiClientSecure netClient;
#else
WiFiClient netClient;
#endif
PubSubClient mqtt(netClient);

// ---------- Topics ----------
String T_TELEMETRY, T_EVENT, T_STATUS_SUB, T_LWT;

// ---------- State ----------
enum EventState { STATE_IDLE, STATE_PENDING, STATE_APPROVED, STATE_IN_PROGRESS, STATE_COMPLETED };
EventState currentState = STATE_IDLE;

unsigned long lastTelemetry = 0;
unsigned long lastHeartbeatBlink = 0;
unsigned long lastWifiAttempt = 0;
unsigned long lastMqttAttempt = 0;
bool lastButtonState = HIGH;
bool otaStarted = false;

// ---------- Offline telemetry buffer ----------
// If Wi-Fi/MQTT drops mid-run (tunnels, dead zones, moving between hotspots),
// we keep the last BUFFER_SIZE GPS/telemetry points in RAM and flush them,
// oldest first, once the connection is back — so gps_logs doesn't have gaps
// during a live emergency.
struct BufferedPoint { double lat; double lng; float speed; uint8_t battery; unsigned long ts; };
#define GPS_BUFFER_SIZE 40
BufferedPoint gpsBuffer[GPS_BUFFER_SIZE];
int bufHead = 0;   // next write position
int bufCount = 0;  // number of valid buffered points

void bufferPush(double lat, double lng, float speed, uint8_t battery) {
  gpsBuffer[bufHead] = { lat, lng, speed, battery, millis() };
  bufHead = (bufHead + 1) % GPS_BUFFER_SIZE;
  if (bufCount < GPS_BUFFER_SIZE) bufCount++;
}

void flushBufferedPoints() {
  if (bufCount == 0 || !mqtt.connected()) return;
  int startIdx = (bufHead - bufCount + GPS_BUFFER_SIZE) % GPS_BUFFER_SIZE;
  for (int i = 0; i < bufCount; i++) {
    int idx = (startIdx + i) % GPS_BUFFER_SIZE;
    BufferedPoint &p = gpsBuffer[idx];
    StaticJsonDocument<256> doc;
    doc["device_id"] = DEVICE_ID;
    doc["lat"] = p.lat; doc["lng"] = p.lng; doc["speed_kmph"] = p.speed;
    doc["battery_pct"] = p.battery; doc["buffered"] = true; doc["captured_ms_ago"] = millis() - p.ts;
    char buf[256];
    size_t n = serializeJson(doc, buf);
    mqtt.publish(T_TELEMETRY.c_str(), buf, n);
  }
  Serial.print("[BUFFER] Flushed "); Serial.print(bufCount); Serial.println(" buffered points");
  bufCount = 0; bufHead = 0;
}

void setup() {
  Serial.begin(115200);
  GPSSerial.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

  pinMode(PIN_BTN_ACTIVATE, INPUT_PULLUP);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_LED_RED, OUTPUT);
  pinMode(PIN_LED_AMBER, OUTPUT);
  pinMode(PIN_LED_GREEN, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);
  setLeds(false, false, false);

  T_TELEMETRY  = String(TOPIC_PREFIX) + "/ambulance/" + DEVICE_ID + "/telemetry";
  T_EVENT      = String(TOPIC_PREFIX) + "/ambulance/" + DEVICE_ID + "/event";
  T_STATUS_SUB = String(TOPIC_PREFIX) + "/ambulance/" + DEVICE_ID + "/status";
  T_LWT        = String(TOPIC_PREFIX) + "/ambulance/" + DEVICE_ID + "/lwt";

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
  } else if (mqtt.connected() && bufCount > 0) {
    flushBufferedPoints();
  }
  if (otaStarted) ArduinoOTA.handle();
  mqtt.loop();

  while (GPSSerial.available() > 0) gps.encode(GPSSerial.read());

  handleActivateButton();

  if (millis() - lastTelemetry > TELEMETRY_INTERVAL_MS) {
    publishTelemetry();
    lastTelemetry = millis();
  }

  updateStatusIndicator();
}

// ---------- OTA (Over-the-Air) updates ----------
// Lets you push firmware fixes to a fielded ambulance unit over Wi-Fi from
// the Arduino IDE (Tools > Port > <device-name at ip>) instead of pulling
// the box to re-flash over USB. Only activates once Wi-Fi is up.
void setupOTA() {
  ArduinoOTA.setHostname((String("secapms-amb-") + DEVICE_ID).c_str());
  ArduinoOTA.onStart([]() { Serial.println("[OTA] Update starting"); });
  ArduinoOTA.onEnd([]() { Serial.println("[OTA] Update complete"); });
  ArduinoOTA.onError([](ota_error_t error) { Serial.printf("[OTA] Error %u\n", error); });
  ArduinoOTA.begin();
  otaStarted = true;
  Serial.println("[OTA] Ready");
}

// ---------- Wi-Fi ----------
void connectWifi() {
  Serial.println("[WiFi] Connecting to " WIFI_SSID " ...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

// ---------- MQTT ----------
void connectMqtt() {
  Serial.println("[MQTT] Connecting...");
  String clientId = String("secapms-amb-") + DEVICE_ID;
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
    mqtt.subscribe(T_STATUS_SUB.c_str());
  } else {
    Serial.print("[MQTT] Failed, rc="); Serial.println(mqtt.state());
  }
}

void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) return;
  String status = doc["status"] | "";
  if (status == "approved") currentState = STATE_APPROVED;
  else if (status == "in_progress") currentState = STATE_IN_PROGRESS;
  else if (status == "completed" || status == "cancelled" || status == "rejected") currentState = STATE_IDLE;
}

// ---------- Activation button ----------
void handleActivateButton() {
  bool reading = digitalRead(PIN_BTN_ACTIVATE);
  if (lastButtonState == HIGH && reading == LOW) { // pressed (falling edge)
    delay(30); // debounce
    if (digitalRead(PIN_BTN_ACTIVATE) == LOW) {
      publishActivateEvent();
    }
  }
  lastButtonState = reading;
}

void publishActivateEvent() {
  if (currentState != STATE_IDLE) return; // already have an active event
  StaticJsonDocument<256> doc;
  doc["device_id"] = DEVICE_ID;
  doc["patient_category"] = "critical"; // panel could have a 3-way switch instead
  doc["lat"] = gps.location.isValid() ? gps.location.lat() : 0.0;
  doc["lng"] = gps.location.isValid() ? gps.location.lng() : 0.0;
  doc["ts"] = millis();
  char buf[256];
  size_t n = serializeJson(doc, buf);
  mqtt.publish(T_EVENT.c_str(), buf, n);
  currentState = STATE_PENDING;
  tone(PIN_BUZZER, 2000, 150);
  Serial.println("[EVENT] Activation published");
}

// ---------- Telemetry ----------
void publishTelemetry() {
  double lat = gps.location.isValid() ? gps.location.lat() : 0.0;
  double lng = gps.location.isValid() ? gps.location.lng() : 0.0;
  float speed = gps.speed.isValid() ? gps.speed.kmph() : 0.0;
  uint8_t battery = readBatteryPercent();

  if (!mqtt.connected()) {
    // No connection right now — keep the point in RAM instead of dropping it.
    bufferPush(lat, lng, speed, battery);
    return;
  }
  StaticJsonDocument<256> doc;
  doc["device_id"] = DEVICE_ID;
  doc["lat"] = lat;
  doc["lng"] = lng;
  doc["speed_kmph"] = speed;
  doc["battery_pct"] = battery;
  doc["rssi"] = WiFi.RSSI();
  doc["gps_fix"] = gps.location.isValid();
  char buf[256];
  size_t n = serializeJson(doc, buf);
  mqtt.publish(T_TELEMETRY.c_str(), buf, n);
}

int readBatteryPercent() {
  int raw = analogRead(PIN_BATTERY_ADC); // 0-4095 on ESP32 ADC1
  float voltage = (raw / 4095.0) * 3.3 * 2.0; // x2 because of the 10k/10k divider
  // Adjust these bounds to your battery chemistry (example: 3S Li-ion 9-12.6V via divider)
  float pct = (voltage - 9.0) / (12.6 - 9.0) * 100.0;
  if (pct < 0) pct = 0; if (pct > 100) pct = 100;
  return (int)pct;
}

// ---------- Indicators ----------
void setLeds(bool r, bool a, bool g) {
  digitalWrite(PIN_LED_RED, r); digitalWrite(PIN_LED_AMBER, a); digitalWrite(PIN_LED_GREEN, g);
}

void updateStatusIndicator() {
  bool blinkOn = (millis() / 400) % 2 == 0;
  switch (currentState) {
    case STATE_IDLE:        setLeds(false, false, false); break;
    case STATE_PENDING:     setLeds(false, blinkOn, false); break;      // blinking amber
    case STATE_APPROVED:
    case STATE_IN_PROGRESS: setLeds(false, false, true); break;         // solid green
    case STATE_COMPLETED:   setLeds(false, false, false); currentState = STATE_IDLE; break;
  }
}
