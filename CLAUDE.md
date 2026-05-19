# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Architecture

Garage-door opener triggered from a phone over MQTT. Four pieces, three of them in this repo:

1. **`index.html` + `sw.js` + `manifest.json` + `icon.svg`** — installable PWA served from the repo root. Reads a bearer token from `?t=...` (persisted to `localStorage` as `garage_token`) and POSTs to the Netlify Function with `Authorization: Bearer <token>`. If no token is stored, shows a setup screen that accepts a pasted setup link. In PWA (`standalone`) mode it **auto-fires a trigger on launch** and re-fires on every `visibilitychange` → `visible` — this is why opening the app from the home screen also opens the door. As a normal web page it shows a manual "Open Garage" button. After publish the UI waits for the ESP32's ack and either shows "Opened / Garage confirmed" or "No reply / Sent — no garage response".
2. **`netlify/functions/trigger.js`** — Netlify Function. Validates the bearer token against `VALID_TOKENS` (comma-separated env var), connects to an MQTTS broker on port 8883 (TLS, cert verification disabled), **subscribes to `garage/ack`**, publishes `1` to `garage/trigger` at QoS 1, and waits up to `ACK_TIMEOUT_MS` for the ESP32 to respond before returning `{ confirmed: true/false }`. CORS is wide open (`*`) because auth is by bearer token. Tunables at the top of the file: `CONNECT_TIMEOUT_MS` (broker connect) and `ACK_TIMEOUT_MS` (wait for ESP32 confirmation).
3. **`esp32/garageButton/garageButton.ino`** — ESP32 firmware. Subscribes to `garage/trigger`, publishes a status string to `garage/ack` on receipt, then drives a servo on GPIO13 to mechanically press the door-opener button. Status LED on GPIO2: fast blink during WiFi connect, slower blink during MQTT connect, solid HIGH when ready, 3-flicker pattern when a trigger fires. WiFi/MQTT creds live in `esp32/garageButton/secrets.h` (gitignored — copy from `secrets.h.example`). A 45 s hardware watchdog reboots the board if anything wedges; per-stage connect budgets (`WIFI_CONNECT_BUDGET_MS`, `MQTT_CONNECT_BUDGET_MS`) restart the board if WiFi or MQTT take too long rather than spinning in a half-broken state.
4. **HiveMQ Cloud broker** (external, not in this repo) — relays between the function and the ESP32 on topics `garage/trigger` and `garage/ack`.

Token revocation = remove the token string from `VALID_TOKENS` in Netlify env vars. There is no per-token identity or audit log; tokens are interchangeable shared secrets.

See **`esp32-setup.md`** for hardware specifics, board/library versions, `arduino-cli` build/flash commands, WiFi/MQTT config notes, and the gotchas list.

## Required environment variables (Netlify)

- `VALID_TOKENS` — comma-separated list of accepted bearer tokens
- `MQTT_HOST` — HiveMQ Cloud broker hostname (port 8883 is hardcoded in the function)
- `MQTT_USER`, `MQTT_PASS` — broker credentials

`rejectUnauthorized: false` is set on the MQTT connection — the broker's TLS cert is not verified.

## Commands

Web app + function:

```bash
npm install              # install mqtt dependency for the function
netlify dev              # local dev: serves index.html + runs the function at /.netlify/functions/trigger
netlify deploy --prod    # ship web + function changes
```

ESP32 firmware (PowerShell) — full setup in `esp32-setup.md`:

```powershell
arduino-cli compile --fqbn esp32:esp32:esp32 "esp32\garageButton"
arduino-cli upload  --fqbn esp32:esp32:esp32 --port COM5 "esp32\garageButton"
```

There are no tests, lint, or build steps for the web side — `netlify.toml` publishes the repo root as-is.
