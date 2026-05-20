# Cooldown, pinned status, and personalized greeting — design

Date: 2026-05-19

## Goals

1. Prevent the garage door from being triggered more than once every 10 seconds, enforced on the ESP32 itself so it's authoritative regardless of who calls the trigger endpoint.
2. Make the "Garage online / offline" status indicator always visible and anchored to a fixed location on the page, so it never disappears or shifts during state transitions or animations.
3. Tighten the button state machine: block clicks when the garage is unreachable, block clicks during a request in flight, and block clicks for 10 s after a click — showing a greyed cooldown countdown on the button itself.
4. When Tarshley opens the app, show a stylish animated greeting banner at the top of the page.

## Non-goals

- No changes to the Netlify Function (`netlify/functions/trigger.js`). It already forwards the ESP32's ack string verbatim in `data.message`.
- No new env vars, no new tokens, no per-token identity on the server side. Tarshley uses an existing valid token; the personalization is purely client-side via a `&u=` query param on her setup link.
- No audit logging of ignored cooldown triggers beyond the ack string itself.

## (1) ESP32 cooldown

In `esp32/garageButton/garageButton.ino`, gate the servo actuation in `onMessage()` on a `(millis() - lastTriggerMs) >= TRIGGER_COOLDOWN_MS` check. Use a `hasTriggered` flag so the first 10 s after boot are not treated as "within cooldown" (since `lastTriggerMs` starts at 0).

```cpp
const unsigned long TRIGGER_COOLDOWN_MS = 10000;
unsigned long lastTriggerMs = 0;
bool hasTriggered = false;
```

On a trigger received during cooldown:
- Publish an ack of the form `"cooldown — ignored, Ns left"` to `garage/ack` (where N is `ceil(remainingMs / 1000)`).
- Return without moving the servo and without running the LED flicker pattern.

On a trigger received outside cooldown:
- Set `hasTriggered = true` and `lastTriggerMs = millis()`.
- Continue with the existing ack + servo + LED flicker flow.

`millis()` overflow (after ~50 days of uptime) is handled correctly by the unsigned subtraction.

## (2) Pinned status indicator

The current code calls `document.body.innerHTML = ...` in `fireTrigger()` and `showButtonUI()`, which wipes the `#conn` element entirely. The fix is to stop touching `body.innerHTML` directly and instead swap content inside a dedicated `#stage` container, while `#conn` lives as a sibling that is never replaced.

DOM structure:

```html
<body>
  <div id="greeting"></div>   <!-- empty unless user === 'tarshley' -->
  <div id="stage"></div>      <!-- setup screen / button UI / sending UI swap in here -->
  <div id="conn">Checking…</div>
</body>
```

CSS for `#conn`:

```css
#conn {
  position: fixed;
  bottom: 24px;
  left: 50%;
  transform: translateX(-50%);
  /* existing color/typography/dot styles preserved */
}
```

All existing `document.body.innerHTML = ...` writes become `document.getElementById('stage').innerHTML = ...`. The status indicator is then visible across every UI state (setup, idle, sending, result, cooldown) and stays anchored at the bottom of the viewport regardless of body content changes.

## (3) Button state machine

States and visuals:

| State | When entered | Button appearance | Clickable |
|---|---|---|---|
| `idle` | garage online, no cooldown active | green "Open Garage" | yes |
| `idle-offline` | `connState !== 'online'` | greyed "Garage offline" | no |
| `sending` | click handler started, fetch in flight | orange "Sending…" | no |
| `result` | response received, displays for 1.5 s | green "Opened" / yellow "No reply" / yellow "Already triggered" (cooldown ack from ESP32) / red "Error" | no |
| `cooldown` | 10 s after click time, transitions in after `result` | greyed "Wait Ns" with N counting down | no |

Detection of the cooldown ack from the ESP32: parse `data.message` for the substring `"cooldown"` (case-insensitive). If present, render the result as "Already triggered" in the yellow `.warn` style.

Cooldown bookkeeping:
- At click time (in the manual button handler), write `localStorage.setItem('garage_last_trigger_ms', String(Date.now()))` before issuing the fetch.
- A helper `cooldownRemainingMs()` returns `Math.max(0, 10000 - (Date.now() - lastTrigger))`.
- On every page load and after each `result` state expires, call `cooldownRemainingMs()`. If `> 0`, enter cooldown state and start a `setInterval` (250 ms tick) that updates the button label to `Wait ${Math.ceil(remainingMs / 1000)}s`. When remaining hits 0, clear the interval and re-enter `idle` (or `idle-offline` based on current `connState`).

Online-status gating:
- `applyConnUI()` already updates `connState`. Extend it so it also calls a `refreshButtonState()` helper that re-evaluates idle vs idle-offline whenever the connection state changes — so the button automatically enables/disables as the garage goes online/offline.

Auto-trigger path (`fireTrigger`) interactions with cooldown:
- Before issuing the fetch, check `cooldownRemainingMs()`. If `> 0`, skip the fetch entirely and render the cooldown state directly. The ESP32 would have ignored the trigger anyway; doing the round-trip just to display "Already triggered" wastes time.
- Online-status gating does NOT apply to auto-trigger. The user explicitly opened the PWA to open the door, and the status feed may not have connected yet — auto-trigger fires optimistically. If the garage is actually offline, the existing `No reply` flow handles it.

## (4) Tarshley greeting

Identification:
- Her setup link is `https://<site>/?t=<her-token>&u=tarshley`.
- The existing setup flow already saves `?t` to `localStorage`. Extend it: when consuming URL params on page load, also read `u` and persist it as `garage_user`. Same treatment in the setup-screen paste flow (parse `u` from the pasted URL).
- On every load, if `localStorage.getItem('garage_user') === 'tarshley'`, render the greeting into `#greeting`. Otherwise leave it empty.

Visual:
- Text: `Hi Tarshley ❤️`.
- Container: `position: fixed; top: 24px; left: 0; right: 0; text-align: center;` so it stays anchored to the top of the viewport and never overlaps with the centered button or the bottom-anchored status indicator.
- Text styling: pink-to-red linear gradient applied via `background-image` + `background-clip: text` + `color: transparent`, with `background-size: 200% 100%` and a `@keyframes` animation that sweeps `background-position` from `0% 0%` to `200% 0%` over ~6 s, infinite.
- Heart: rendered as a separate inline element (so the gradient doesn't apply to the emoji), with a `@keyframes pulse` doing `transform: scale(1) → scale(1.15) → scale(1)` over ~1.5 s, infinite, `ease-in-out`.
- Font: bold, ~1.4 rem, letter-spacing slightly tightened.

## Files changed

- `esp32/garageButton/garageButton.ino` — cooldown gate in `onMessage()`.
- `index.html` — DOM restructure (persistent `#conn`, new `#stage`, optional `#greeting`), `#stage`-based content swaps, cooldown state machine, `connState`-driven button gating, greeting renderer + styles, `u` query param ingestion.

## Tunables introduced

- `TRIGGER_COOLDOWN_MS = 10000` in firmware.
- `COOLDOWN_MS = 10000` in `index.html` (kept in sync with firmware by convention — no shared source).
