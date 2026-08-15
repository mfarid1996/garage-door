# ESP32 Garage Door — Setup Notes

## How it works

**Flow:** Browser → Netlify Function (validates token, holds MQTT creds) → HiveMQ Cloud broker → ESP32

**Web app:** https://garage-door-mfarid.netlify.app — tap the button on an enrolled device to trigger.

**To enroll a new device:** generate a token, add it to the `VALID_TOKENS` Netlify env var (comma-separated), redeploy, then open `https://garage-door-mfarid.netlify.app/?t=<token>` on the new device once to save it silently.

---

## Guest access

There is one shared, permanent guest token in `VALID_TOKENS`, prefixed `guest-` so it is
identifiable in the list. Hand the same link to every guest:

```
https://garage-door-mfarid.netlify.app/?t=guest-<...>
```

Guests do **not** install anything — the link opens in whatever browser they already have and
shows the same button you get. The token is saved to `localStorage` and stripped from the address
bar on first load, so reopening the link later still works.

A guest token is functionally identical to a personal one: no expiry, no usage cap, no per-guest
identity, and no audit trail. It is a permanent key. Anyone who keeps or forwards the link can open
the door indefinitely.

**To revoke all guest access at once** (the only revocation granularity — every guest shares this
token, so rotating it cuts off all of them):

```powershell
# read current value, drop the guest- entry, append a fresh one, redeploy
netlify env:list --json      # find the guest- entry
netlify env:set VALID_TOKENS "<other,tokens>,guest-<new-random>" --force
netlify deploy --prod
```

Generate the random part in PowerShell (note: the static `RandomNumberGenerator.GetBytes` does not
exist in Windows PowerShell 5.1 — use `RNGCryptoServiceProvider`):

```powershell
$rng = New-Object System.Security.Cryptography.RNGCryptoServiceProvider
$bytes = New-Object byte[] 18; $rng.GetBytes($bytes)
"guest-" + [Convert]::ToBase64String($bytes).Replace('+','-').Replace('/','_').TrimEnd('=')
```

---

## Hardware

- **Board:** ESP32-D0WD-V3 (revision v3.1), dual-core 240MHz
- **USB-serial chip:** CH340 (VID_1A86 / PID_7523)
- **Built-in LED:** GPIO2
- **Amazon listing:** https://www.amazon.com/dp/B0B18JQF16
- **Servo:** SG90 (180° positional, not continuous rotation)
  - Black → GND, Red → VIN (5V), Yellow → GPIO13
  - On trigger: rotates from 45° (rest) to 10°, holds 150 ms, returns to 45° — see the `SERVO_*` constants in `garageButton.ino`
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
arduino-cli board list    # find the port — see the warning below
arduino-cli compile --fqbn esp32:esp32:esp32 "<path-to-repo>\esp32\garageButton"
arduino-cli upload --fqbn esp32:esp32:esp32 --port COM3 "<path-to-repo>\esp32\garageButton"
```

**The COM port moves.** Windows assigns it per physical USB socket, so plugging the board into a
different port changes the number (it has been COM5 and COM3 at different times). Always run
`arduino-cli board list` first and use the port on the `USB-SERIAL CH340` row.

`compile` needs no board attached — only `upload` does.

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
- **Port:** 8883 (MQTT over TLS), 8884 (WSS, used by the browser status feed)
- **Topics:**
  | Topic | Retained | Published by | Purpose |
  |---|---|---|---|
  | `garage/trigger` | no | Netlify Function | `1` = press the button |
  | `garage/ack` | no | ESP32 | trigger received / cooldown-ignored |
  | `garage/status` | **yes** | ESP32 (+ LWT) | `online` / `offline` |
  | `garage/session` | **yes** | ESP32 | `{session, boots, downMs, reason}` on every MQTT connect |
- Credentials go in `secrets.h` (ESP32) and Netlify env vars (`MQTT_HOST`, `MQTT_USER`, `MQTT_PASS`)
- If you rotate the MQTT password, update both Netlify env vars and re-flash the ESP32

### Session / boot counters (NVS)

The firmware keeps two monotonic counters in NVS namespace `garage`: `boots` (bumped once per
`setup()`) and `session` (bumped on every successful MQTT connect). They are written **only** on
those two events — never periodically — because a heartbeat write would burn the flash sector for
no extra information. A full flash erase resets them to 0; the backend resyncs after one poll and
loses at most one recorded reconnect.

Note that **flashing the board is itself a disconnect**, so every upload registers as one
connectivity-loss event in the statistics.

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
