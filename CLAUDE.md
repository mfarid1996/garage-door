# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Architecture

Single-page web app that triggers a garage door opener over MQTT. Three pieces:

1. **`index.html`** — static page served from the repo root. Reads a bearer token from `?t=...` (persisted to `localStorage` as `garage_token`) and POSTs to the Netlify Function with `Authorization: Bearer <token>`. If no token is stored, the UI is replaced with a "not authorised" message. Access is distributed by sharing URLs of the form `https://<site>/?t=<token>`.
2. **`netlify/functions/trigger.js`** — Netlify Function that validates the bearer token against `VALID_TOKENS` (comma-separated env var), connects to an MQTTS broker on port 8883, publishes `1` to topic `garage/trigger` with QoS 1, and resolves. 8-second timeout. CORS is wide open (`*`) because auth is by bearer token.
3. **MQTT broker → ESP/relay** (not in this repo) — subscribes to `garage/trigger` and pulses the door opener.

Token revocation = remove the token string from `VALID_TOKENS` in Netlify env vars. There is no per-token identity or audit log; tokens are interchangeable shared secrets.

## Required environment variables (Netlify)

- `VALID_TOKENS` — comma-separated list of accepted bearer tokens
- `MQTT_HOST` — broker hostname (port 8883 is hardcoded)
- `MQTT_USER`, `MQTT_PASS` — broker credentials

`rejectUnauthorized: false` is set on the MQTT connection — the broker's TLS cert is not verified.

## Commands

```bash
npm install              # install mqtt dependency for the function
netlify dev              # local dev: serves index.html + runs the function at /.netlify/functions/trigger
```

There are no tests, lint, or build steps — `netlify.toml` publishes the repo root as-is.
