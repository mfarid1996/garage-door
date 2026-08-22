import { getStore } from '@netlify/blobs';

// Records that a trigger was requested, so the PWA can show "N clicks" alongside
// the connectivity numbers.
//
// WHY THIS IS A SEPARATE FUNCTION rather than a few lines inside trigger.js:
//
//  * trigger.js is a runtime-API-v1 (CommonJS) function, and v1 lambda-compat
//    functions do not get the Netlify Blobs context injected — getStore() throws
//    MissingBlobsEnvironmentError. That is the exact bug that silently broke the
//    stats feature once already. Recording from there would mean migrating it.
//  * trigger.js is the garage-critical path. Click counting is telemetry. Keeping
//    them apart means a fault in the counter can never stop the door opening — the
//    browser fires this call and ignores the result.
//
// The trade-off: this counts clicks the PWA made, not every request that reached
// /trigger. A curl straight to /trigger would open the door without being counted.
// That is the right side to err on given what trigger.js is.
//
// Runtime API v2 (ESM, `export default`) and the .mjs extension are both load-bearing;
// see the comment at the top of poll-status.mjs.

const STORE_NAME = 'garage-stats';
const CLICKS_KEY = 'clicks';
const RETENTION_MS = 8 * 24 * 60 * 60 * 1000;  // matches the poller's event retention
const MAX_CLICKS = 5000;                        // bound the blob if something spams

const CORS = {
  'Access-Control-Allow-Origin': '*',
  'Access-Control-Allow-Headers': 'Authorization',
  'Access-Control-Allow-Methods': 'POST, OPTIONS',
  'Cache-Control': 'no-store',
};

const json = (body, status = 200) => new Response(JSON.stringify(body), {
  status,
  headers: { ...CORS, 'Content-Type': 'application/json' },
});

export default async (req) => {
  if (req.method === 'OPTIONS') return new Response('', { status: 200, headers: CORS });
  if (req.method !== 'POST') return new Response('Method Not Allowed', { status: 405, headers: CORS });

  const auth = req.headers.get('authorization') ?? '';
  const token = auth.startsWith('Bearer ') ? auth.slice(7) : null;
  const valid = (process.env.VALID_TOKENS ?? '').split(',').map(t => t.trim()).filter(Boolean);
  // Guest tokens count too: a guest opening the door is still a door opening.
  if (!token || !valid.includes(token)) return json({ error: 'Unauthorized' }, 401);

  const now = Date.now();

  try {
    const store = getStore({ name: STORE_NAME, consistency: 'strong' });

    let clicks = [];
    try {
      const raw = await store.get(CLICKS_KEY, { type: 'json' });
      if (raw && Array.isArray(raw.clicks)) clicks = raw.clicks.filter(Number.isFinite);
    } catch {
      // A read failure must not wipe the log: fall through with what we have and
      // accept losing this one click rather than truncating history.
      return json({ ok: false, reason: 'blob read failed' });
    }

    clicks.push(now);
    // Prune to the retention window, keep chronological order, then cap.
    clicks = clicks.filter(t => t >= now - RETENTION_MS).sort((a, b) => a - b);
    if (clicks.length > MAX_CLICKS) clicks = clicks.slice(clicks.length - MAX_CLICKS);

    // Read-modify-write, so two clicks landing in the same instant can lose one.
    // The firmware enforces a 10 s cooldown and the UI a matching one, so genuine
    // concurrency is rare, and undercounting by one is a better failure than
    // blocking a door open on a write conflict.
    await store.setJSON(CLICKS_KEY, { v: 1, clicks });

    return json({ ok: true, t: now, total: clicks.length });
  } catch (err) {
    console.error('record-click failed:', err && err.message);
    return json({ ok: false, error: err && err.message ? err.message : 'unknown error' });
  }
};
