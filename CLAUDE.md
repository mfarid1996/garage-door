# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Architecture

Garage-door opener triggered from a phone over MQTT. Five pieces, four of them in this repo:

1. **`index.html` + `sw.js` + `manifest.json` + `icon.svg`** — installable PWA served from the repo root. Reads a bearer token from `?t=...` (persisted to `localStorage` as `garage_token`) and POSTs to the Netlify Function with `Authorization: Bearer <token>`. If no token is stored, shows a setup screen that accepts a pasted setup link. Shows a manual "Open Garage" button (in both PWA `standalone` mode and as a normal web page); the trigger fires **only when the button is tapped** — opening the app does not open the door. After publish the UI waits for the ESP32's ack and either shows "Opened / Garage confirmed" or "No reply / Sent — no garage response".
2. **`netlify/functions/trigger.js`** — Netlify Function. Validates the bearer token against `VALID_TOKENS` (comma-separated env var), connects to an MQTTS broker on port 8883 (TLS, cert verification disabled), **subscribes to `garage/ack`**, publishes `1` to `garage/trigger` at QoS 1, and waits up to `ACK_TIMEOUT_MS` for the ESP32 to respond before returning `{ confirmed: true/false }`. CORS is wide open (`*`) because auth is by bearer token. Tunables at the top of the file: `CONNECT_TIMEOUT_MS` (broker connect) and `ACK_TIMEOUT_MS` (wait for ESP32 confirmation).
3. **`netlify/functions/poll-status.js` + `netlify/functions/stats.js`** — connectivity history. See "Downtime statistics" below.
4. **`esp32/garageButton/garageButton.ino`** — ESP32 firmware. Subscribes to `garage/trigger`, publishes a status string to `garage/ack` on receipt, then drives a servo on GPIO13 to mechanically press the door-opener button. Status LED on GPIO2: fast blink during WiFi connect, slower blink during MQTT connect, solid HIGH when ready, 3-flicker pattern when a trigger fires. WiFi/MQTT creds live in `esp32/garageButton/secrets.h` (gitignored — copy from `secrets.h.example`). A 45 s hardware watchdog reboots the board if anything wedges; per-stage connect budgets (`WIFI_CONNECT_BUDGET_MS`, `MQTT_CONNECT_BUDGET_MS`) restart the board if WiFi or MQTT take too long rather than spinning in a half-broken state.
5. **HiveMQ Cloud broker** (external, not in this repo) — relays between the functions and the ESP32 on topics `garage/trigger`, `garage/ack`, `garage/status`, and `garage/session`.

## Button states

The button is gated on `connState`, which is driven by the retained `garage/status` topic over the browser's WSS subscription:

| `connState` | Button | Status text |
|---|---|---|
| `online` | green, clickable | `Ready` |
| `unknown` | **lighter green (`#57d894`), clickable** | `Ready (unverified)` |
| `offline` | greyed, **disabled** | `Garage offline` |
| — (within 10 s of a tap) | greyed, disabled | `Wait Ns` |

`unknown` means the status feed has not spoken yet (or the WSS socket closed, or the `wss-creds` fetch failed) — that is *not* evidence the garage is down, so the tap is allowed through. A **confirmed** `offline` is real information and does block the tap. The 10 s cooldown starts at click time regardless of outcome, and the ESP32 enforces its own authoritative 10 s cooldown independently.

## Downtime statistics

The PWA shows ESP32 connectivity loss counts and cumulative downtime over 24 h and 7 d, between the button and the `#conn` indicator. History is recorded by two cooperating halves, because neither alone is sufficient — the device cannot report while it is offline, and a 1-minute poller cannot see sub-minute blips:

- **`garage/session`** (retained) — the ESP32 publishes `{"session":N,"boots":N,"downMs":N,"reason":"boot|wifi|mqtt"}` after *every* successful MQTT connect. `session` and `boots` are NVS-persisted monotonic counters, written **only** on those two discrete events — never on a timer, deliberately, to avoid flash wear. `downMs` is the exact `millis()`-measured outage length, or `-1` after a boot (millis() reset, so the duration is genuinely unknowable).
- **`netlify/functions/poll-status.mjs`** — scheduled every minute via the inline `export const config = { schedule }`. Reads the retained `garage/status` + `garage/session`, appends transitions to the `garage-stats` Netlify Blobs store, prunes to 8 days. A rising `session` counter reveals reconnects the poller slept through; events dedupe on that counter. **A gap between polls is an observability gap, never downtime** — poller events are stamped at observation time and never backdated, so a skipped scheduled run cannot be billed as garage downtime.
- **`netlify/functions/stats.mjs`** — GET, bearer-token gated, returns the raw event log plus a `ready` flag. **All window math happens in the browser**, so the 24 h / 7 d windows can change without a redeploy or a re-flash. Outages are clipped to the window edge; an ongoing outage counts up to now.

### Why the stats functions are `.mjs` and the others are `.js`

**Do not "tidy" these into `.js` — it silently breaks them.** `trigger.js` and `wss-creds.js` are runtime-API-**v1** functions (CommonJS, `exports.handler`). v1 lambda-compat functions **do not get the Netlify Blobs context injected**, so `getStore()` throws `MissingBlobsEnvironmentError` at runtime. The stats pair therefore uses runtime API **v2** (`export default`, returns a `Response`), which does get the context.

v2 requires ES modules, and `package.json` has no `"type": "module"`, so a `.js` file here would be parsed as CommonJS and `export default` would be a syntax error. Hence `.mjs`, which is always ESM. The two runtime versions coexist fine — Netlify records `runtimeAPIVersion` per function.

This bit us once already: both stats functions shipped as v1 `.js`, `getStore()` threw on every call, and because `stats.js` caught the error and degraded to an empty payload, the UI showed a harmless-looking "No connectivity data yet" instead of an error. The `ready` flag now distinguishes the two.

Events are `{t, type:'down'|'up', downMs, src:'device'|'poller', session}`. A post-boot reconnect is recorded as a zero-length outage: the *count* is right and no duration is invented. This means reboot downtime (~15–20 s each) is under-reported by design. Stats are hidden for `guest-` tokens.

Token revocation = remove the token string from `VALID_TOKENS` in Netlify env vars, then redeploy (env changes do not reach deployed functions until the next deploy). There is no per-token identity or audit log; tokens are interchangeable shared secrets.

One entry in `VALID_TOKENS` is prefixed `guest-` — a single shared, permanent guest key handed to every visitor. It is an ordinary token in every respect; the prefix exists only so it can be found and rotated. Rotating it revokes all guests at once, which is the only revocation granularity guests have. See the "Guest access" section of `esp32-setup.md`.

See **`esp32-setup.md`** for hardware specifics, board/library versions, `arduino-cli` build/flash commands, WiFi/MQTT config notes, and the gotchas list.

## Required environment variables (Netlify)

- `VALID_TOKENS` — comma-separated list of accepted bearer tokens
- `MQTT_HOST` — HiveMQ Cloud broker hostname (port 8883 is hardcoded in the function)
- `MQTT_USER`, `MQTT_PASS` — broker credentials

`rejectUnauthorized: false` is set on the MQTT connection — the broker's TLS cert is not verified.

No new env vars were needed for the statistics feature. Netlify Blobs (`@netlify/blobs`) injects its own `siteID`/`token` automatically inside Functions, so the `garage-stats` store needs no configuration.

## Commands

Web app + function:

```bash
npm install              # install mqtt dependency for the function
netlify dev              # local dev: serves index.html + runs the function at /.netlify/functions/trigger
netlify deploy --prod    # ship web + function changes
```

ESP32 firmware (PowerShell) — full setup in `esp32-setup.md`:

```powershell
arduino-cli board list                                              # find the port first — it moves
arduino-cli compile --fqbn esp32:esp32:esp32 "esp32\garageButton"
arduino-cli upload  --fqbn esp32:esp32:esp32 --port COM3 "esp32\garageButton"
```

**The COM port is not stable** — it is assigned per USB port, so it changes whenever the board is moved to a different socket. Always run `arduino-cli board list` and look for the `USB-SERIAL CH340` entry rather than trusting a hardcoded number.

`arduino-cli compile` works with no board attached, so firmware changes can be verified without hardware; only `upload` needs the board.

There are no tests, lint, or build steps for the web side — `netlify.toml` publishes the repo root as-is.
