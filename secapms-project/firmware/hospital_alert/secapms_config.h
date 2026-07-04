/*
  secapms_config.h
  ------------------------------------------------------------
  Shared Wi-Fi + MQTT configuration for every SECAPMS device
  (ambulance unit, junction controller, hospital alert panel).

  Copy this file next to each .ino sketch (already done in this
  repo layout) and edit the values below before flashing.

  There is NO SIM/GSM module anywhere in this design. Every device
  talks over Wi-Fi only, to an MQTT broker reachable on your
  network or over the internet (see README.md for broker options).
  ------------------------------------------------------------
*/

#pragma once

// ---------- Wi-Fi ----------
#define WIFI_SSID        "YOUR_WIFI_SSID"
#define WIFI_PASSWORD    "YOUR_WIFI_PASSWORD"

// ---------- MQTT broker ----------
// Local broker example (Mosquitto on a Raspberry Pi / laptop on the same LAN):
//   #define MQTT_HOST      "192.168.1.50"
//   #define MQTT_PORT      1883
//   #define MQTT_USE_TLS   0
//
// Cloud broker example (HiveMQ Cloud, free tier, TLS required):
//   #define MQTT_HOST      "xxxxxxx.s1.eu.hivemq.cloud"
//   #define MQTT_PORT      8883
//   #define MQTT_USE_TLS   1
#define MQTT_HOST        "192.168.1.50"
#define MQTT_PORT        1883
#define MQTT_USE_TLS     0
#define MQTT_USERNAME    "secapms_device"
#define MQTT_PASSWORD    "change_me"

// ---------- Topic namespace ----------
// All topics are namespaced under secapms/<version>/... so this
// firmware can share a broker with other projects safely.
#define TOPIC_PREFIX      "secapms/v1"

// Device -> cloud (published)
//   secapms/v1/ambulance/<unit_id>/telemetry     (lat,lng,speed,battery,rssi every ~3s)
//   secapms/v1/ambulance/<unit_id>/event          (activation button press)
//   secapms/v1/junction/<junction_id>/health      (heartbeat: online, uptime, rssi)
//   secapms/v1/hospital/<hospital_id>/ack         (READY button press on alert panel)
//   secapms/v1/<device_kind>/<id>/lwt             (Last Will: "offline" if device drops)
//
// Cloud -> device (subscribed)
//   secapms/v1/junction/<junction_id>/command     ("priority_green" | "normal" | "override")
//   secapms/v1/hospital/<hospital_id>/alert       (JSON: incoming ambulance details)
//   secapms/v1/ambulance/<unit_id>/status         (JSON: approved/rejected/in_progress)

// ---------- Device identity ----------
// Give every physical unit a unique, stable ID matching the `external_id`
// you register in the SECAPMS `devices` table (e.g. "AU-4521", "JC-ANN",
// "HA-STA"). This is what ties the ESP32 to a DB row.
#define DEVICE_ID         "CHANGE_ME_UNIQUE_ID"

// ---------- Timing ----------
#define TELEMETRY_INTERVAL_MS   3000
#define HEARTBEAT_INTERVAL_MS   10000
#define WIFI_RETRY_MS           5000
#define MQTT_RETRY_MS           5000
