# SECAPMS Firmware — ESP32, Wi-Fi only (no SIM/GSM)

Real, complete Arduino/ESP32 firmware for the three physical device types
in the SECAPMS schema (`ambulance_unit`, `junction_controller`,
`hospital_alert`). Every device connects over Wi-Fi and speaks MQTT —
there is no SIM/GSM module anywhere in this design.

## Bill of materials

| Device | Core parts |
|---|---|
| Ambulance unit | ESP32 DevKit v1, NEO-6M GPS module, 1 push button, active buzzer, 3 LEDs (or RGB), 2x 10k resistors (battery divider) |
| Junction controller | ESP32 DevKit v1, 2-channel opto-isolated relay module, 1 status LED |
| Hospital alert panel | ESP32 DevKit v1, 16x2 I2C LCD, active buzzer, 3 LEDs, 1 push button |

## Wiring

**Ambulance unit** (`ambulance_unit/ambulance_unit.ino`)
```
NEO-6M TX  -> GPIO16 (RX2)      NEO-6M VCC -> 3.3V   NEO-6M GND -> GND
ACTIVATE button -> GPIO4 (to GND, internal pullup)
Buzzer -> GPIO25   LED Red -> GPIO26   LED Amber -> GPIO27   LED Green -> GPIO14
Battery sense (10k/10k divider) -> GPIO34
```

**Junction controller** (`junction_controller/junction_controller.ino`)
```
Relay IN1 (force green)      -> GPIO26
Relay IN2 (hold cross red)   -> GPIO27
Status LED                   -> GPIO14
Relay board VCC/GND          -> 5V / GND from cabinet low-voltage rail
```
⚠️ **Safety**: this drives a relay into your *existing* signal controller's low-voltage override input — never wire an ESP32 directly into mains-side equipment. Any real junction install needs your local traffic authority's sign-off, a certified interposing relay panel, and a physical manual override at the cabinet.

**Hospital alert panel** (`hospital_alert/hospital_alert.ino`)
```
LCD SDA -> GPIO21   LCD SCL -> GPIO22 (I2C, address 0x27 or 0x3F)
Buzzer -> GPIO25   LED Red -> GPIO26   LED Amber -> GPIO27   LED Green -> GPIO14
READY button -> GPIO4 (to GND, internal pullup)
```

## Software setup (Arduino IDE)

1. Install [Arduino IDE 2.x](https://www.arduino.cc/en/software).
2. **File → Preferences → Additional Board URLs**, add:
   `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
3. **Tools → Board → Boards Manager**, search "esp32", install the Espressif package.
4. **Tools → Manage Libraries**, install:
   - `PubSubClient` (Nick O'Leary)
   - `ArduinoJson` (Benoit Blanchon, v6 or v7)
   - `TinyGPSPlus` (Mikal Hart) — ambulance unit only
   - `LiquidCrystal_I2C` — hospital alert panel only
5. Open the relevant folder — each contains the `.ino` sketch plus its own copy of `secapms_config.h` (Arduino requires the header alongside the sketch).
6. Edit `secapms_config.h` in that folder: `WIFI_SSID`, `WIFI_PASSWORD`, `MQTT_HOST`/`MQTT_PORT`/`MQTT_USE_TLS`, and a unique `DEVICE_ID` matching the `external_id` you register in the `devices` table (e.g. `AU-4521`, `JC-ANN`, `HA-STA`).
7. **Tools → Board**, select "ESP32 Dev Module", select the correct port, click **Upload**. Open Serial Monitor at 115200 baud to watch connection logs.

## MQTT broker options
- **Local bench testing**: [Mosquitto](https://mosquitto.org/download/) on a laptop/Raspberry Pi on the same Wi-Fi. Port 1883, no TLS. Watch traffic with `mosquitto_sub -h <ip> -t 'secapms/#' -v`.
- **Real pilot**: [HiveMQ Cloud](https://www.hivemq.com/mqtt-cloud-broker/) free tier — TLS endpoint reachable from anywhere. Set `MQTT_PORT 8883`, `MQTT_USE_TLS 1` in the config header.

Topic layout is documented at the top of `common/secapms_config.h`.

## Built-in hardening
- **Offline GPS buffering** (ambulance unit): the last 40 telemetry points are kept in a RAM ring buffer and flushed the moment MQTT reconnects, so a brief dead zone doesn't leave gaps in `gps_logs`.
- **OTA updates** (all three): once on Wi-Fi, each device also advertises for `ArduinoOTA` — Arduino IDE → Tools → Port → look for `secapms-amb-<id>` / `secapms-jc-<id>` / `secapms-ha-<id>` under "Network Ports" — so firmware fixes can be pushed without pulling hardware for a USB re-flash. Add `ArduinoOTA.setPassword(...)` before fielding real units.

## Next step
Once devices are online and publishing, point `../backend` at the same broker — that service is what turns these MQTT messages into database rows and back. See `../backend/README.md`.
