import { connect } from 'mqtt';
import { getStore } from '@netlify/blobs';

// Runtime API v2 (ESM, `export default`), NOT the v1 `exports.handler` form used by
// trigger.js and wss-creds.js. That is deliberate: v1 lambda-compat functions do not
// get the Netlify Blobs context injected, so getStore() throws
// MissingBlobsEnvironmentError at runtime. v2 functions do. The two styles coexist
// fine - Netlify records runtimeAPIVersion per function - so the garage-critical v1
// functions were left alone.
//
// The .mjs extension is load-bearing: package.json has no "type":"module", so a .js
// file here would be parsed as CommonJS and `export default` would be a syntax error.
//
// The schedule lives in the inline config below rather than in netlify.toml, which is
// the v2 idiom.

const STORE_NAME = 'garage-stats';
const STATE_KEY = 'state';

const CONNECT_TIMEOUT_MS = 6000;
const COLLECT_MS = 3000;         // window to receive the retained messages
const HARD_TIMEOUT_MS = 15000;   // absolute cap (scheduled functions get 30s)
const POLL_INTERVAL_MS = 60000;  // must match the schedule below
const MISSED_POLL_MS = 3 * POLL_INTERVAL_MS;
const RETENTION_MS = 8 * 24 * 60 * 60 * 1000;
const MAX_EVENTS = 5000;         // hard cap so a flapping device can't grow the blob without bound

const blankState = () => ({
  v: 1,
  since: null,        // epochMs of the first observation we ever made
  lastPollMs: null,   // epochMs of the last poll that actually observed garage/status
  status: 'unknown',  // last observed garage/status ('online' | 'offline' | 'unknown')
  statusSince: null,  // epochMs we first observed the current status
  session: null,      // last seen garage/session session counter
  boots: null,        // last seen garage/session boots counter
  bootsAttributed: null, // highest `boots` already reported on a recorded event
  events: [],
});

const readState = async (store) => {
  let raw = null;
  try {
    raw = await store.get(STATE_KEY, { type: 'json' });
  } catch {
    // A read failure must not wipe history: bail out by returning null so the
    // caller skips this poll entirely instead of overwriting a good log.
    return null;
  }
  if (!raw || typeof raw !== 'object') return blankState();
  const state = { ...blankState(), ...raw };
  state.events = Array.isArray(raw.events) ? raw.events.filter(e => e && typeof e.t === 'number') : [];
  if (state.status !== 'online' && state.status !== 'offline') state.status = 'unknown';
  return state;
};

// Connect once, grab the retained payloads of garage/status + garage/session,
// disconnect. Never rejects - failures come back as { error }.
const collectRetained = () => new Promise((resolve) => {
  const out = { status: null, session: null, error: null };
  let done = false;
  let client = null;
  let connectTimer = null;
  let collectTimer = null;
  let hardTimer = null;

  const finish = (error) => {
    if (done) return;
    done = true;
    if (error && !out.error) out.error = error;
    clearTimeout(connectTimer);
    clearTimeout(collectTimer);
    clearTimeout(hardTimer);
    try { if (client) client.end(true); } catch { /* already gone */ }
    resolve(out);
  };

  if (!process.env.MQTT_HOST) return finish('MQTT_HOST not set');

  hardTimer = setTimeout(() => finish('Hard timeout'), HARD_TIMEOUT_MS);

  try {
    client = connect(`mqtts://${process.env.MQTT_HOST}`, {
      port: 8883,
      username: process.env.MQTT_USER,
      password: process.env.MQTT_PASS,
      rejectUnauthorized: false,
      clean: true,
      reconnectPeriod: 0, // one shot: never silently reconnect inside a 1-minute poll
      connectTimeout: CONNECT_TIMEOUT_MS,
      clientId: `garage-poller-${Math.random().toString(16).slice(2, 10)}`,
    });
  } catch (err) {
    return finish(err.message);
  }

  connectTimer = setTimeout(() => finish('Connect timeout'), CONNECT_TIMEOUT_MS);

  client.on('connect', () => {
    clearTimeout(connectTimer);
    client.subscribe(['garage/status', 'garage/session'], { qos: 1 }, (subErr) => {
      if (subErr) return finish(subErr.message);
      // Retained messages arrive right after SUBACK; garage/session may not exist
      // yet (older firmware), so always wait out the full window.
      collectTimer = setTimeout(() => finish(null), COLLECT_MS);
    });
  });

  client.on('message', (topic, payload) => {
    const text = payload.toString();
    if (topic === 'garage/status') out.status = text;
    else if (topic === 'garage/session') out.session = text;
    if (out.status !== null && out.session !== null) finish(null);
  });

  client.on('error', (err) => finish(err.message));
});

export const parseSession = (text) => {
  const out = { session: null, boots: null, downMs: null, reason: null, rst: null };
  if (!text) return out;
  try {
    const p = JSON.parse(text);
    if (!p || typeof p !== 'object') return out;
    if (Number.isFinite(p.session)) out.session = Math.trunc(p.session);
    if (Number.isFinite(p.boots)) out.boots = Math.trunc(p.boots);
    if (Number.isFinite(p.downMs)) out.downMs = Math.trunc(p.downMs);
    if (typeof p.reason === 'string') out.reason = p.reason;
    // Absent on pre-Nano-ESP32 firmware; stays null and nothing downstream cares.
    //
    // WHITELISTED, not merely type-checked: this value is broker-controlled and ends
    // up inside the PWA's DOM. Anyone able to publish a retained garage/session could
    // otherwise store an <img onerror> here and steal the door token of every
    // privileged viewer. resetReasonName() only ever emits short lowercase words, so
    // nothing legitimate is lost. Defence in depth — the frontend escapes as well.
    if (typeof p.rst === 'string' && /^[a-z]{2,12}$/.test(p.rst)) out.rst = p.rst;
  } catch {
    // Malformed retained payload: ignore it rather than throwing.
  }
  return out;
};

// `meta` carries the device's own account of a reconnect and is attached only to
// the 'up' event, so counting events that have an `rst` counts reboots exactly once.
//
// The subtle part: esp_reset_reason() is LATCHED AT BOOT and the firmware republishes
// it unchanged on every subsequent reconnect. So `rst` describes *this* event only
// when reason === 'boot'. Attaching it to a wifi/mqtt reconnect would re-report a
// single old brownout as one brownout per reconnect, forever.
export const mkEvent = (t, type, downMs, src, session, meta = null) => {
  const ev = {
    t: Math.trunc(t),
    type,
    downMs: downMs == null ? null : Math.trunc(downMs),
    src,
    session: session == null ? null : Math.trunc(session),
  };
  if (meta && meta.reason) ev.reason = meta.reason;
  if (meta && meta.rst) ev.rst = meta.rst;
  if (meta && Number.isFinite(meta.reboots) && meta.reboots > 0) ev.reboots = meta.reboots;
  return ev;
};

// The gate described above, isolated so it can be tested directly. Returns the meta
// to hang on this reconnect's 'up' event, or null if there is nothing to say.
//
// `reboots` is the exact count from the device's monotonic NVS `boots` counter, and
// it is what makes the tally honest. `rst` alone undercounts badly, because the
// retained garage/session holds only the NEWEST connect:
//   * boot, then a WiFi blip before the next poll -> newest reason is 'wifi', the
//     gate correctly drops the stale latched rst, and the reboot vanishes entirely.
//     That is the exact signature of a mains cut that also took the router down.
//   * three brownout resets inside one poll interval -> one retained payload -> the
//     reboot loop this feature exists to catch reads as a single reboot.
// The counter survives both: it is differenced against what we last attributed, so
// the total is right even when only the most recent cause is knowable.
export const makeUpMeta = (deviceReconnect, reason, rst, reboots = null) =>
  (deviceReconnect ? { reason, rst: reason === 'boot' ? rst : null, reboots } : null);

// The log alternates down/up. Refusing a repeat of the last type makes the whole
// function idempotent, which matters because Netlify can fire a scheduled
// function more than once for the same slot.
const push = (list, ev) => {
  if (list.length && list[list.length - 1].type === ev.type) return false;
  list.push(ev);
  return true;
};

export default async () => {
  const now = Date.now();
  const ok = (body) => new Response(JSON.stringify(body), {
    status: 200,
    headers: { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' },
  });

  try {
    // siteID/token are injected automatically inside v2 Functions.
    // Strong consistency because eventual reads can lag up to ~60s - exactly our
    // poll interval - which would make us re-read our own stale snapshot.
    const store = getStore({ name: STORE_NAME, consistency: 'strong' });

    const obs = await collectRetained();

    // Broker unreachable, or no retained garage/status. We learned NOTHING this
    // minute, so we write nothing at all: leaving lastPollMs/status/events
    // untouched keeps a broker outage from being recorded as garage downtime and
    // keeps a bad poll from corrupting the stored log.
    if (obs.error || obs.status === null) {
      console.warn('poll-status: no observation', obs.error ?? 'no retained garage/status');
      return ok({ ok: false, reason: obs.error ?? 'no retained garage/status' });
    }

    const curStatus = obs.status === 'online' ? 'online' : obs.status === 'offline' ? 'offline' : null;
    if (!curStatus) {
      console.warn('poll-status: unexpected garage/status payload', obs.status);
      return ok({ ok: false, reason: 'unexpected garage/status payload' });
    }

    const state = await readState(store);
    if (!state) return ok({ ok: false, reason: 'blob read failed' });

    const prevStatus = state.status;
    const { session, boots, downMs, reason, rst } = parseSession(obs.session);

    // --- missed-poll accounting -------------------------------------------
    // Scheduled runs get delayed, skipped, or doubled by the platform. A gap
    // between polls is an OBSERVABILITY gap, not evidence that the garage was
    // down, so:
    //   * every poller-derived event is stamped at `now` and NEVER backdated to
    //     lastPollMs - backdating would silently bill the whole gap as downtime;
    //   * the only thing allowed to describe what happened inside a gap is the
    //     device itself, via the garage/session counter + its exact downMs.
    const gapMs = state.lastPollMs == null ? null : now - state.lastPollMs;
    const missedPolls = gapMs == null ? 0 : Math.max(0, Math.round(gapMs / POLL_INTERVAL_MS) - 1);
    if (gapMs != null && gapMs > MISSED_POLL_MS) {
      console.warn(`poll-status: ${missedPolls} poll(s) missed (gap ${gapMs}ms) - not counted as downtime`);
    }

    const events = state.events.slice();
    const baseline = prevStatus !== 'online' && prevStatus !== 'offline';

    // A session counter that went UP means the device reconnected at least once
    // since the last poll. Dedupe on the counter itself so the same reconnect is
    // never recorded twice (duplicate scheduled runs, retries, replays).
    const sessionIncreased = session != null && state.session != null && session > state.session;
    const alreadyRecorded = session != null && events.some(e => e.src === 'device' && e.session === session);
    const deviceReconnect = sessionIncreased && !alreadyRecorded;
    // downMs === -1 means "unknowable" (this connect followed a boot, millis()
    // had reset). Anything outside the retention window is nonsense - drop it.
    const exactDownMs = downMs != null && downMs >= 0 && downMs <= RETENTION_MS ? downMs : null;

    // Reboots we have not yet attributed to an event. Deliberately differenced
    // against `bootsAttributed` rather than `boots`: `boots` tracks the newest value
    // merely SEEN, and a device that flaps up and back down inside one poll interval
    // bumps it while still reading 'offline', where no 'up' event is recorded. Using
    // `boots` there would consume the increment and lose those reboots for good.
    // Falls back to `boots` for state written before this field existed.
    const attributedBase = Number.isFinite(state.bootsAttributed) ? state.bootsAttributed
                         : (Number.isFinite(state.boots) ? state.boots : null);
    // A drop means NVS was erased (re-flash): resync silently rather than reporting
    // a negative or absurd count.
    const bootsDelta = boots != null && attributedBase != null && boots > attributedBase
                     ? boots - attributedBase : null;

    // Attached to whichever 'up' event this reconnect produces. Gated on
    // deviceReconnect (not on src) because `src` records where the *duration* came
    // from: a post-boot reconnect reports downMs -1, so its duration falls back to
    // the poller while the reason for the reboot is still known exactly.
    const upMeta = makeUpMeta(deviceReconnect, reason, rst, bootsDelta);

    const recorded = [];
    const record = (ev) => { if (push(events, ev)) recorded.push(ev); };

    if (baseline) {
      // First observation ever (or after a state reset): nothing to compare
      // against, so establish a baseline without inventing events. If a log
      // survived the reset and it disagrees with what we can see right now,
      // resync it so the down/up alternation stays intact (otherwise the next
      // real transition would look like a duplicate and get dropped).
      const lastType = events.length ? events[events.length - 1].type : null;
      const impliedType = curStatus === 'online' ? 'up' : 'down';
      if (lastType && lastType !== impliedType) {
        record(mkEvent(now, impliedType, null, 'poller', session));
      }
    } else if (prevStatus === 'online' && curStatus === 'online') {
      if (deviceReconnect) {
        if (exactDownMs != null) {
          // Sub-minute blip that polling slept straight through. The device
          // measured it exactly, so we may backdate the start.
          record(mkEvent(now - exactDownMs, 'down', exactDownMs, 'device', session));
          record(mkEvent(now, 'up', exactDownMs, 'device', session, upMeta));
        } else {
          // Reconnect confirmed but its duration is unknowable (post-boot).
          // Record a zero-length outage: the count is right and we never invent
          // a duration we cannot justify.
          record(mkEvent(now, 'down', null, 'device', session));
          record(mkEvent(now, 'up', null, 'device', session, upMeta));
        }
      }
    } else if (prevStatus === 'online' && curStatus === 'offline') {
      if (deviceReconnect && exactDownMs != null) {
        // It blipped and then died again inside the gap: log the measured blip
        // first, then the outage we can see right now.
        record(mkEvent(now - exactDownMs, 'down', exactDownMs, 'device', session));
        record(mkEvent(now, 'up', exactDownMs, 'device', session, upMeta));
      }
      record(mkEvent(now, 'down', null, 'poller', session));
    } else if (prevStatus === 'offline' && curStatus === 'online') {
      // It came back. If the device just told us exactly how long it was gone,
      // that beats our coarse poll timestamps.
      const useDevice = deviceReconnect && exactDownMs != null;
      record(mkEvent(now, 'up', useDevice ? exactDownMs : null, useDevice ? 'device' : 'poller', session, upMeta));
    } else {
      // Still offline. A session bump here means it flapped up and back down
      // inside the gap; we cannot place those transitions in time, so the
      // standing "down" event is left alone (the counter is still consumed
      // below, so it is never replayed).
    }

    // Prune to the 8-day retention window, keep chronological order (sort is
    // stable, so same-millisecond down/up pairs keep their insertion order).
    const cutoff = now - RETENTION_MS;
    let kept = events.filter(e => e && typeof e.t === 'number' && e.t >= cutoff);
    kept.sort((a, b) => a.t - b.t);
    if (kept.length > MAX_EVENTS) kept = kept.slice(kept.length - MAX_EVENTS);

    // Advance the attribution watermark only when a recorded event actually carried
    // the count, so an unrecorded increment stays owed and is reported later.
    const attributedNow = recorded.some(e => e.reboots) && boots != null
                        ? boots
                        : (attributedBase != null ? attributedBase : boots);

    const next = {
      v: 1,
      since: state.since ?? now,
      lastPollMs: now,
      status: curStatus,
      statusSince: prevStatus === curStatus && state.statusSince != null ? state.statusSince : now,
      session: session != null ? session : state.session,
      boots: boots != null ? boots : state.boots,
      bootsAttributed: attributedNow != null ? attributedNow : null,
      events: kept,
    };

    await store.setJSON(STATE_KEY, next);

    return ok({
      ok: true,
      status: curStatus,
      session,
      boots,
      reason,
      rst,
      downMs,
      missedPolls,
      recorded: recorded.length,
      events: kept.length,
    });
  } catch (err) {
    // A scheduled function must never throw: that would just retry blindly.
    console.error('poll-status failed:', err && err.message);
    return ok({ ok: false, error: err && err.message ? err.message : 'unknown error' });
  }
};

export const config = {
  schedule: '* * * * *',
};
