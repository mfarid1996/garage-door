# ESP32 Garage Door — Setup Notes

## How it works

**Flow:** Browser → Netlify Function (validates token, holds MQTT creds) → HiveMQ Cloud broker → ESP32

**Web app:** https://garage-door-mfarid.netlify.app — tap the button on an enrolled device to trigger.

**To enroll a new device:** generate a token, add it to the `VALID_TOKENS` Netlify env var (comma-separated), redeploy, then open `https://garage-door-mfarid.netlify.app/?t=<token>` on the new device once to save it silently.

---

## Hardware

- **Board:** ESP32-D0WD-V3 (revision v3.1), dual-core 240MHz
- **USB-serial chip:** CH340 (VID_1A86 / PID_7523)
- **Built-in LED:** GPIO2
- **Amazon listing:** https://www.amazon.com/dp/B0B18JQF16
- **Servo:** SG90 (180° positional, not continuous rotation)
  - Black → GND, Red → VIN (5V), Yellow → GPIO13
  - On trigger: rotates from 90° to 120° (+30°), waits 1s, returns to 90°
  - Library: ESP32Servo (install via `arduino-cli lib install "ESP32Servo"`)

---

## Software

- **Arduino CLI:** v1.4.1, installed via `winget install ArduinoSA.CLI`
- **ESP32 Arduino core:** `esp32:esp32`, version 3.3.8
- **Board FQBN:** `esp32:esp32:esp32`
- **Libraries:** PubSubClient 2.8.0, ESP32Servo
- **Node.js:** v24.15.0 (installed via winget)
- **Netlify CLI:** v26.0.1 (installed via npm)

### Compile & upload

First, copy `esp32/garageButton/secrets.h.example` to `esp32/garageButton/secrets.h` and fill in your WiFi and MQTT credentials.

```powershell
arduino-cli compile --fqbn esp32:esp32:esp32 "<path-to-repo>\esp32\garageButton"
arduino-cli upload --fqbn esp32:esp32:esp32 --port COM5 "<path-to-repo>\esp32\garageButton"
```

### Netlify redeploy (after any change to web app or env vars)

```powershell
$env:PATH = [System.Environment]::GetEnvironmentVariable("PATH","Machine") + ";" + [System.Environment]::GetEnvironmentVariable("PATH","User")
netlify deploy --prod
```

---

## WiFi

- **Band:** 2.4GHz, channel 11 — ESP32 supports 2.4GHz only
- **Note:** SSID is case-sensitive
- WiFi credentials go in `secrets.h` (gitignored)

---

## MQTT (HiveMQ Cloud)

- **Broker:** HiveMQ Cloud (free tier)
- **Port:** 8883 (MQTT over TLS)
- **Topic:** `garage/trigger`
- Credentials go in `secrets.h` (ESP32) and Netlify env vars (`MQTT_HOST`, `MQTT_USER`, `MQTT_PASS`)
- If you rotate the MQTT password, update both Netlify env vars and re-flash the ESP32

---

## Netlify env vars

Set these in the Netlify dashboard for the `garage-door-mfarid` project:

| Variable       | Value                          |
|----------------|-------------------------------|
| `MQTT_HOST`    | your HiveMQ broker hostname   |
| `MQTT_USER`    | your MQTT username            |
| `MQTT_PASS`    | your MQTT password            |
| `VALID_TOKENS` | comma-separated device tokens |

---

## Gotchas

1. **SSID case sensitivity** — WiFi.begin() silently fails with status 6 (WL_DISCONNECTED) if the case is wrong.
2. **CH340 resets on serial open** — Opening COM port toggles DTR, rebooting the board. Set `DtrEnable = false` in PowerShell if you don't want a reset.
3. **Serial output timing** — setup() runs immediately at boot. `delay(3000)` at the start of setup() gives time to open the serial monitor.
4. **PATH refresh in PowerShell** — After installing tools via winget/npm, refresh PATH:
   ```powershell
   $env:PATH = [System.Environment]::GetEnvironmentVariable("PATH","Machine") + ";" + [System.Environment]::GetEnvironmentVariable("PATH","User")
   ```
5. **PowerShell `&&` not supported** — Use `; if ($?) { ... }` to chain commands conditionally.
6. **SG90 is positional (0–180°), not continuous rotation** — cannot do a full 360°. Adjust mechanically.
7. **Netlify CLI needs PowerShell PATH** — netlify is installed via npm into the Windows PATH, not the bash PATH. Run netlify commands via PowerShell.
