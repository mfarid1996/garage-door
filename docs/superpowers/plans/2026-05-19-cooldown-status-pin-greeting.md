# Cooldown, pinned status, and greeting — implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a 10 s servo-trigger cooldown on the ESP32, pin the garage-status indicator to a fixed viewport position so it's always visible, gate the web button on the garage's online status + a client-side 10 s cooldown countdown, and show an animated "Hi Tarshley ❤️" greeting when her token is in use.

**Architecture:**
- ESP32 enforces the cooldown authoritatively in `onMessage()` and replies with a `"cooldown - ignored, Ns left"` ack on suppressed triggers.
- Web app restructures its DOM so the `#conn` (online/offline) indicator is a persistent sibling of a swappable `#stage` container, positioned `fixed` at the bottom of the viewport. The button gains a 5-state machine driven by `connState` + a localStorage-backed cooldown timer.
- Tarshley's personalization is purely client-side: a `&u=tarshley` query param on her setup link, persisted to `localStorage.garage_user`, drives an animated banner at the top of the page.

**Tech Stack:** Arduino C++ (ESP32 + PubSubClient + ESP32Servo), vanilla JS / CSS in `index.html`, no test framework on either side.

**Spec:** `docs/superpowers/specs/2026-05-19-cooldown-status-pin-greeting-design.md`

**Note on TDD:** Neither codebase has a test framework, so "verify" steps are compile + manual exercise rather than automated assertions. This is honest about the constraints of an Arduino + static-HTML project; the design itself is small enough to verify by direct interaction.

## File map

- `esp32/garageButton/garageButton.ino` — modified in Task 1 only (cooldown constants + gate in `onMessage`).
- `index.html` — modified in Tasks 2, 3, 4 (DOM restructure → button state machine → greeting).
- No new files. No changes to `netlify/functions/trigger.js`, `sw.js`, `manifest.json`, or `secrets.h`.

---

### Task 1: ESP32 — 10 s servo-trigger cooldown

**Files:**
- Modify: `esp32/garageButton/garageButton.ino:32-52`

- [ ] **Step 1.1: Add cooldown globals**

In `esp32/garageButton/garageButton.ino`, immediately after the existing line `const int SERVO_MOVE_MS     = 200;` (around line 32) and before the `void onMessage(...)` declaration (around line 34), add:

```cpp
// Servo-trigger cooldown — ignore repeat triggers within this window to prevent
// double-actuation when the PWA auto-fires on visibilitychange or the user mashes.
const unsigned long TRIGGER_COOLDOWN_MS = 10000;
unsigned long lastTriggerMs = 0;
bool hasTriggered = false;
```

- [ ] **Step 1.2: Gate `onMessage` on the cooldown**

Replace the entire body of `void onMessage(char* topic, byte* payload, unsigned int len)` with:

```cpp
void onMessage(char* topic, byte* payload, unsigned int len) {
  if (hasTriggered && millis() - lastTriggerMs < TRIGGER_COOLDOWN_MS) {
    unsigned long remainingMs = TRIGGER_COOLDOWN_MS - (millis() - lastTriggerMs);
    unsigned long remainingS  = (remainingMs + 999) / 1000;
    char ackMsg[64];
    snprintf(ackMsg, sizeof(ackMsg), "cooldown - ignored, %lus left", remainingS);
    mqtt.publish("garage/ack", ackMsg);
    Serial.printf("Trigger ignored — %lus cooldown remaining\n", remainingS);
    return;
  }
  hasTriggered = true;
  lastTriggerMs = millis();

  char ackMsg[64];
  snprintf(ackMsg, sizeof(ackMsg), "trigger received, uptime %lus", millis() / 1000);
  mqtt.publish("garage/ack", ackMsg);  // immediate ack on receive

  servo.write(SERVO_TRIGGER_POS);
  delay(SERVO_MOVE_MS);
  delay(SERVO_HOLD_MS);
  servo.write(SERVO_REST_POS);
  delay(SERVO_MOVE_MS);

  // 3 quick flicker-offs against the solid-on baseline, ending back at solid on.
  for (int i = 0; i < 3; i++) {
    digitalWrite(LED_PIN, LOW);  delay(120);
    digitalWrite(LED_PIN, HIGH); delay(120);
  }

  Serial.println("Trigger received — servo actuated, LED blinked");
}
```

Notes:
- `hasTriggered` flag exists so the first 10 s of uptime are not in cooldown — `lastTriggerMs == 0` would otherwise put `millis() - 0 < 10000` true until uptime reaches 10 s.
- ASCII hyphen (`-`), not em-dash, in the ack string — keeps the source file ASCII-clean and avoids any UTF-8 ambiguity with `snprintf`.

- [ ] **Step 1.3: Compile**

Run from the repo root:

```powershell
arduino-cli compile --fqbn esp32:esp32:esp32 "esp32\garageButton"
```

Expected: exits 0, no errors. Sketch size should be roughly the same as before (~1.2 MB).

- [ ] **Step 1.4: Flash & smoke-test (manual)**

```powershell
arduino-cli upload --fqbn esp32:esp32:esp32 --port COM5 "esp32\garageButton"
```

Then in a browser at the deployed site (or via `netlify dev` if you've already done Task 2+):
1. Click "Open Garage" once → servo actuates, web shows "Opened".
2. Within 10 s, click again → servo does NOT actuate. Web shows whatever the existing UI does (verifying this is a Task 3 concern; for now just confirm the servo stays still and serial monitor prints `Trigger ignored — Ns cooldown remaining`).
3. Wait 10+ s, click → servo actuates again.

Skip Step 1.4 if hardware isn't accessible; Step 1.3 (compile) is sufficient to unblock the rest of the plan.

- [ ] **Step 1.5: Commit**

```bash
git add esp32/garageButton/garageButton.ino
git commit -m "Add 10s servo-trigger cooldown on ESP32"
```

---

### Task 2: Web — persistent `#conn` + `#stage` restructure

**Files:**
- Modify: `index.html` (CSS in `<style>` block, HTML in `<body>`, and all `document.body.innerHTML = ...` sites in `<script>`)

The goal: stop wiping the DOM wholesale. After this task the page has three persistent top-level elements (`#greeting`, `#stage`, `#conn`), and content swaps happen inside `#stage` only.

- [ ] **Step 2.1: Update body CSS to stop relying on flex-gap layout**

In `index.html`, replace the existing `body { ... }` rule (lines ~11-21) with:

```css
body {
  display: flex;
  flex-direction: column;
  justify-content: center;
  align-items: center;
  min-height: 100dvh;
  background: #111;
  font-family: -apple-system, BlinkMacSystemFont, sans-serif;
  color: #fff;
}
#stage {
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 28px;
}
```

Rationale: the existing layout uses `body { gap: 28px }` to space the button, status, and conn together. After restructuring, the button + status live in `#stage` and `#conn` is fixed-positioned, so the gap belongs on `#stage`.

- [ ] **Step 2.2: Pin `#conn` to the bottom of the viewport**

Replace the existing `#conn { ... }` rule (lines ~43-52) with:

```css
#conn {
  position: fixed;
  bottom: 24px;
  left: 50%;
  transform: translateX(-50%);
  font-size: 0.7rem;
  color: #666;
  letter-spacing: 0.04em;
  text-transform: uppercase;
  display: flex;
  align-items: center;
  gap: 8px;
}
```

The `margin-top: -16px` from the original is removed (no longer needed — element is no longer in flex flow). The other `#conn::before`, `#conn.online::before`, `#conn.offline::before` rules immediately following stay unchanged.

- [ ] **Step 2.3: Add static `#stage` and `#conn` to the HTML body**

Replace the existing `<body>` open + script start (lines ~99-100) with:

```html
<body>
  <div id="stage"></div>
  <div id="conn">Checking…</div>
  <script>
```

Now `#stage` and `#conn` are guaranteed to exist throughout the app's lifetime.

- [ ] **Step 2.4: Swap content into `#stage` instead of `body`**

There are three sites that do `document.body.innerHTML = ...`. Each needs to become `document.getElementById('stage').innerHTML = ...`, and the `<div id="conn">...</div>` line must be removed from the injected HTML (it now lives statically).

**Site 1** — `showSetupScreen()` (around lines 189-197). Replace its `document.body.innerHTML = \`...\`` block with:

```js
      document.getElementById('stage').innerHTML = `
        <div id="setup-wrap">
          <strong>Device not set up</strong>
          <p>Copy your setup link from the message Mark sent you, then paste it below.</p>
          <input id="setup-input" type="url" placeholder="Paste your setup link…" autocomplete="off" autocorrect="off" spellcheck="false">
          <button id="setup-btn">Save</button>
          <div id="setup-err"></div>
        </div>`;
```

**Site 2** — `showButtonUI()` (around lines 217-222). Replace its `document.body.innerHTML = \`...\`` block with:

```js
      document.getElementById('stage').innerHTML = `
        <button id="btn">Open Garage</button>
        <div id="status">Ready</div>`;
      applyConnUI();
```

(Note: `<div id="conn">Checking…</div>` is removed from the template — that element is now static.)

**Site 3** — `fireTrigger()` (around lines 276-278). Replace its `document.body.innerHTML = \`...\`` block with:

```js
      document.getElementById('stage').innerHTML = `
        <button id="btn" class="sending">Sending…</button>
        <div id="status"></div>`;
```

- [ ] **Step 2.5: Verify with `netlify dev`**

```bash
netlify dev
```

Open the dev URL in a non-PWA browser tab. Open it again with `?t=<your-test-token>` if needed. Confirm:
1. `#conn` is visible at the bottom-center of the viewport on the setup screen.
2. `#conn` stays in the same screen position after entering the button UI.
3. Clicking the button shows "Sending…" / "Opened" / "No reply" without `#conn` disappearing or shifting.
4. Resizing the browser keeps `#conn` anchored to the bottom-center.

- [ ] **Step 2.6: Commit**

```bash
git add index.html
git commit -m "Pin garage-status indicator to fixed viewport position"
```

---

### Task 3: Web — button state machine, cooldown countdown, online gating, auto-trigger skip

**Files:**
- Modify: `index.html` (CSS for cooldown button style, JS for state machine)

After this task the button reflects 5 states (idle / idle-offline / sending / result / cooldown), can't be clicked when the garage isn't online or while a request is in flight or during the 10 s cooldown, and the auto-trigger path (`fireTrigger`) skips the fetch when in cooldown.

- [ ] **Step 3.1: Add cooldown button CSS**

In `index.html`, immediately after the existing `#btn.error { ... }` rule (around line 41) add:

```css
#btn.cooldown {
  background: #444;
  color: #999;
  box-shadow: 0 0 20px rgba(0,0,0,0.4);
  cursor: not-allowed;
}
#btn:disabled { cursor: not-allowed; }
```

- [ ] **Step 3.2: Add cooldown constants and helper, and a centralized button-state function**

In `index.html`, inside the `<script>` block, immediately after the existing `let statusFeedStarted = false;` line (around line 117) add:

```js
const COOLDOWN_MS = 10000;
let buttonState = null;            // 'idle' | 'idle-offline' | 'sending' | 'result' | 'cooldown'
let cooldownTimer = null;

function cooldownRemainingMs() {
  const last = parseInt(localStorage.getItem('garage_last_trigger_ms') || '0', 10);
  if (!last) return 0;
  return Math.max(0, COOLDOWN_MS - (Date.now() - last));
}

function applyButtonState() {
  // Sending and result are transient — they manage themselves and call applyButtonState() when done.
  if (buttonState === 'sending' || buttonState === 'result') return;

  const btn    = document.getElementById('btn');
  const status = document.getElementById('status');
  if (!btn || !status) return;  // setup screen is showing — nothing to gate

  if (cooldownTimer) { clearInterval(cooldownTimer); cooldownTimer = null; }
  btn.classList.remove('sending', 'done', 'warn', 'error', 'cooldown');

  const remaining = cooldownRemainingMs();
  if (remaining > 0) {
    buttonState = 'cooldown';
    btn.classList.add('cooldown');
    btn.disabled = true;
    btn.textContent = 'Wait ' + Math.ceil(remaining / 1000) + 's';
    status.textContent = '';
    cooldownTimer = setInterval(() => {
      const rem = cooldownRemainingMs();
      if (rem <= 0) {
        clearInterval(cooldownTimer);
        cooldownTimer = null;
        buttonState = null;
        applyButtonState();
        return;
      }
      btn.textContent = 'Wait ' + Math.ceil(rem / 1000) + 's';
    }, 250);
    return;
  }

  if (connState === 'online') {
    buttonState = 'idle';
    btn.disabled = false;
    btn.textContent = 'Open Garage';
    status.textContent = 'Ready';
  } else {
    buttonState = 'idle-offline';
    btn.classList.add('cooldown');  // reuse greyed style
    btn.disabled = true;
    btn.textContent = 'Garage offline';
    status.textContent = '';
  }
}
```

- [ ] **Step 3.3: Make `applyConnUI` re-evaluate the button**

Replace the existing `applyConnUI()` function (around lines 140-146) with:

```js
    function applyConnUI() {
      const el = document.getElementById('conn');
      if (el) {
        if (connState === 'online')       { el.textContent = 'Garage online';  el.className = 'online';  }
        else if (connState === 'offline') { el.textContent = 'Garage offline'; el.className = 'offline'; }
        else                              { el.textContent = 'Checking…';      el.className = '';        }
      }
      applyButtonState();
    }
```

- [ ] **Step 3.4: Rewrite the manual click handler to use the state machine**

Replace the entire `showButtonUI()` function (around lines 217-271) with:

```js
    function showButtonUI() {
      document.getElementById('stage').innerHTML = `
        <button id="btn">Open Garage</button>
        <div id="status">Ready</div>`;

      const btn = document.getElementById('btn');
      btn.addEventListener('click', onButtonClick);
      applyButtonState();
    }

    async function onButtonClick() {
      if (buttonState !== 'idle') return;

      const btn    = document.getElementById('btn');
      const status = document.getElementById('status');

      buttonState = 'sending';
      btn.classList.remove('done', 'warn', 'error', 'cooldown');
      btn.classList.add('sending');
      btn.disabled = true;
      btn.textContent = 'Sending…';
      status.textContent = '';

      localStorage.setItem('garage_last_trigger_ms', String(Date.now()));

      let resultClass = '', resultText = '', resultStatus = '';
      try {
        const res = await fetch('/.netlify/functions/trigger', {
          method: 'POST',
          headers: { 'Authorization': 'Bearer ' + token },
        });

        if (res.ok) {
          const data = await res.json().catch(() => ({}));
          const msg = (data.message || '').toLowerCase();
          if (data.confirmed && msg.includes('cooldown')) {
            resultClass  = 'warn';
            resultText   = 'Already triggered';
            resultStatus = data.message;
          } else if (data.confirmed) {
            resultClass  = 'done';
            resultText   = 'Opened';
            resultStatus = 'ESP32 confirmed' + (data.message ? ': ' + data.message : '');
          } else {
            resultClass  = 'warn';
            resultText   = 'No reply';
            resultStatus = 'Sent — no garage response';
          }
        } else if (res.status === 401) {
          resultClass  = 'error';
          resultText   = 'Not Authorised';
          resultStatus = 'Token rejected';
        } else {
          throw new Error('HTTP ' + res.status);
        }
      } catch (err) {
        resultClass  = 'error';
        resultText   = 'Error';
        resultStatus = err.message;
      }

      buttonState = 'result';
      btn.classList.remove('sending');
      btn.classList.add(resultClass);
      btn.textContent = resultText;
      status.textContent = resultStatus;

      setTimeout(() => {
        buttonState = null;
        applyButtonState();
      }, 1500);
    }
```

- [ ] **Step 3.5: Make `fireTrigger` skip the fetch during cooldown and share the same result handling**

Replace the entire `fireTrigger(myGen)` function (around lines 273-317) with:

```js
    async function fireTrigger(myGen) {
      if (gen !== myGen) return;

      // In cooldown? Skip the fetch entirely — ESP32 would ignore it anyway. Render the cooldown UI directly.
      if (cooldownRemainingMs() > 0) {
        showButtonUI();
        return;
      }

      document.getElementById('stage').innerHTML = `
        <button id="btn" class="sending">Sending…</button>
        <div id="status"></div>`;
      const btn    = document.getElementById('btn');
      const status = document.getElementById('status');
      buttonState = 'sending';

      localStorage.setItem('garage_last_trigger_ms', String(Date.now()));

      let resultClass = '', resultText = '', resultStatus = '';
      try {
        const res = await fetch('/.netlify/functions/trigger', {
          method: 'POST',
          headers: { 'Authorization': 'Bearer ' + token },
        });

        if (gen !== myGen) return;

        if (res.ok) {
          const data = await res.json().catch(() => ({}));
          const msg = (data.message || '').toLowerCase();
          if (data.confirmed && msg.includes('cooldown')) {
            resultClass  = 'warn';
            resultText   = 'Already triggered';
            resultStatus = data.message;
          } else if (data.confirmed) {
            resultClass  = 'done';
            resultText   = 'Opened';
            resultStatus = 'Garage confirmed';
          } else {
            resultClass  = 'warn';
            resultText   = 'No reply';
            resultStatus = 'Sent — no garage response';
          }
        } else if (res.status === 401) {
          resultClass  = 'error';
          resultText   = 'Not Authorised';
          resultStatus = 'Token rejected';
        } else {
          throw new Error('HTTP ' + res.status);
        }
      } catch (err) {
        if (gen !== myGen) return;
        resultClass  = 'error';
        resultText   = 'Error';
        resultStatus = err.message;
      }

      buttonState = 'result';
      btn.classList.remove('sending');
      btn.classList.add(resultClass);
      btn.textContent = resultText;
      status.textContent = resultStatus;

      setTimeout(() => {
        if (gen !== myGen) return;
        showButtonUI();
      }, 2000);
    }
```

Notes:
- After the 2 s result hold, `fireTrigger` calls `showButtonUI()` (which calls `applyButtonState()`), so if cooldown is still active the UI naturally transitions to the cooldown countdown.
- `showButtonUI()` is also what happens when `fireTrigger` is short-circuited by the cooldown check at the top — that path renders the idle/offline/cooldown button correctly via `applyButtonState`.

- [ ] **Step 3.6: Verify with `netlify dev`**

Run `netlify dev` and open the page in a regular (non-PWA) browser tab.

1. **Online gating:** Wait for the status feed to connect (`#conn` shows "Garage online"). Button becomes green/active. Then disconnect your ESP32 (or simulate offline by editing `connState = 'offline'; applyConnUI()` in devtools) — button immediately greys to "Garage offline" and becomes unclickable.
2. **Sending gate:** Click the button — it goes orange "Sending…" and a second click in that window does nothing.
3. **Result → cooldown:** After the response, button shows result for 1.5 s, then becomes greyed "Wait 8s" → "Wait 7s" → … → "Wait 1s", then returns to "Open Garage".
4. **Auto-trigger cooldown skip:** Click once, then quickly switch to PWA mode (or simulate `fireTrigger(++gen)` in devtools console) within 10 s — no fetch is made and the cooldown UI is shown.
5. **Cross-reload persistence:** Click, then reload the page within 10 s. The cooldown countdown should resume where it left off (since `garage_last_trigger_ms` is in localStorage).

- [ ] **Step 3.7: Commit**

```bash
git add index.html
git commit -m "Gate button on garage status and 10s client-side cooldown"
```

---

### Task 4: Web — Tarshley greeting

**Files:**
- Modify: `index.html` (CSS for greeting + animations, HTML for `#greeting` slot, JS for `u` param ingestion + renderer)

- [ ] **Step 4.1: Add greeting CSS and keyframes**

In `index.html`, immediately before the closing `</style>` tag (around line 97), add:

```css
#greeting {
  position: fixed;
  top: 24px;
  left: 0;
  right: 0;
  text-align: center;
  font-size: 1.4rem;
  font-weight: 700;
  letter-spacing: 0.02em;
  pointer-events: none;
}
#greeting .greeting-text {
  background: linear-gradient(90deg, #ff5e8a, #ff2d6f, #ff8fa3, #ff5e8a);
  background-size: 200% 100%;
  -webkit-background-clip: text;
          background-clip: text;
  color: transparent;
  animation: greeting-gradient 6s ease-in-out infinite;
}
#greeting .greeting-heart {
  display: inline-block;
  margin-left: 0.25em;
  animation: greeting-pulse 1.5s ease-in-out infinite;
}
@keyframes greeting-gradient {
  0%   { background-position:   0% 50%; }
  50%  { background-position: 200% 50%; }
  100% { background-position:   0% 50%; }
}
@keyframes greeting-pulse {
  0%, 100% { transform: scale(1); }
  50%      { transform: scale(1.18); }
}
```

- [ ] **Step 4.2: Add the static `#greeting` slot to the body**

Replace the body open from Task 2 (the `<div id="stage"></div><div id="conn">…</div>` pair) with the greeting slot prepended:

```html
<body>
  <div id="greeting" hidden></div>
  <div id="stage"></div>
  <div id="conn">Checking…</div>
  <script>
```

- [ ] **Step 4.3: Persist `?u=` from the URL and render the greeting on load**

In `index.html`, replace the existing URL-param block at the top of the script (around lines 101-108) with:

```js
    const params = new URLSearchParams(location.search);
    const urlToken = params.get('t');
    const urlUser  = params.get('u');
    if (urlToken) {
      localStorage.setItem('garage_token', urlToken);
    }
    if (urlUser) {
      localStorage.setItem('garage_user', urlUser.toLowerCase());
    }
    if (urlToken || urlUser) {
      history.replaceState(null, '', location.pathname);
    }

    let token = localStorage.getItem('garage_token');

    renderGreeting();
```

Then immediately after the existing `function applyConnUI() { ... }` (or anywhere reachable from top-level code), add:

```js
    function renderGreeting() {
      const el = document.getElementById('greeting');
      if (!el) return;
      const user = (localStorage.getItem('garage_user') || '').toLowerCase();
      if (user === 'tarshley') {
        el.innerHTML = '<span class="greeting-text">Hi Tarshley</span><span class="greeting-heart">❤️</span>';
        el.hidden = false;
      } else {
        el.innerHTML = '';
        el.hidden = true;
      }
    }
```

- [ ] **Step 4.4: Also ingest `u` when the setup screen consumes a pasted link**

In `index.html`, replace the existing `tryToken(val)` inner function in `showSetupScreen()` (around lines 202-211) with:

```js
      function tryToken(val) {
        val = val.trim();
        let tok = null;
        let user = null;
        try {
          const parsed = new URL(val);
          tok  = parsed.searchParams.get('t');
          user = parsed.searchParams.get('u');
        } catch {}
        if (!tok && val.length > 8 && !val.includes(' ')) tok = val;
        if (!tok) { errEl.textContent = 'Invalid link — make sure you copied the whole URL.'; return; }
        localStorage.setItem('garage_token', tok);
        if (user) localStorage.setItem('garage_user', user.toLowerCase());
        token = tok;
        renderGreeting();
        initApp();
      }
```

- [ ] **Step 4.5: Verify with `netlify dev`**

```bash
netlify dev
```

1. Open `http://localhost:8888/?t=<test-token>&u=tarshley`. Greeting appears at the top of the page with gradient-sweeping text and a pulsing heart. Then reload `http://localhost:8888/` (no params) — greeting still appears, because `garage_user` is persisted in localStorage.
2. In devtools, run `localStorage.removeItem('garage_user')` and reload. Greeting disappears.
3. Open `http://localhost:8888/?t=<test-token>&u=someoneelse` and reload — greeting does not appear.
4. Open the setup screen flow (clear localStorage entirely first, then paste a setup-style URL containing `&u=tarshley`). After Save, greeting appears.
5. Confirm the greeting doesn't shift the button position (button stays vertically centered) and doesn't overlap the bottom `#conn` indicator.

- [ ] **Step 4.6: Commit**

```bash
git add index.html
git commit -m "Show animated 'Hi Tarshley' greeting when ?u=tarshley"
```

---

## Post-implementation

- Update Tarshley's setup link to include `&u=tarshley`. (Send her the new link; the persisted token + user combination will then activate the greeting on her device.)
- Optional: regenerate the deployed PWA via `netlify deploy --prod` once all four tasks are committed and you've verified the flow against the live ESP32.
