import { getStore } from '@netlify/blobs';

// Runtime API v2 (ESM, `export default`) for the same reason as poll-status.mjs:
// v1 `exports.handler` functions do not get the Netlify Blobs context injected, so
// getStore() throws MissingBlobsEnvironmentError at runtime. See the comment there.
// Default path is unchanged: /.netlify/functions/stats
//
// Read-only view of what poll-status.mjs has recorded. All window math (24h, 7d)
// is done in the browser from these raw events, so the windows can be changed later
// without a redeploy or a re-flash.

const STORE_NAME = 'garage-stats';
const STATE_KEY = 'state';
const RETENTION_MS = 8 * 24 * 60 * 60 * 1000;

const CORS = {
  'Access-Control-Allow-Origin': '*',
  'Access-Control-Allow-Headers': 'Authorization',
  'Access-Control-Allow-Methods': 'GET, OPTIONS',
  'Cache-Control': 'no-store',
};

const json = (body, status = 200) => new Response(JSON.stringify(body), {
  status,
  headers: { ...CORS, 'Content-Type': 'application/json' },
});

export default async (req) => {
  if (req.method === 'OPTIONS') return new Response('', { status: 200, headers: CORS });
  if (req.method !== 'GET') return new Response('Method Not Allowed', { status: 405, headers: CORS });

  const auth = req.headers.get('authorization') ?? '';
  const token = auth.startsWith('Bearer ') ? auth.slice(7) : null;
  const valid = (process.env.VALID_TOKENS ?? '').split(',').map(t => t.trim()).filter(Boolean);

  if (!token || !valid.includes(token)) return json({ error: 'Unauthorized' }, 401);

  const now = Date.now();

  try {
    // Strong consistency so a stats request made right after a poll doesn't read
    // a stale snapshot. siteID/token are injected automatically in v2 Functions.
    const store = getStore({ name: STORE_NAME, consistency: 'strong' });
    const state = await store.get(STATE_KEY, { type: 'json' });

    if (!state || typeof state !== 'object') {
      // Poller has never written anything yet (first deploy, or blobs empty).
      // `ready:true` distinguishes this from the error path below - the UI needs
      // to tell "nothing has happened yet" apart from "the backend is broken".
      return json({ now, since: now, events: [], current: { state: 'unknown', since: null }, ready: true });
    }

    const cutoff = now - RETENTION_MS;
    const events = (Array.isArray(state.events) ? state.events : [])
      .filter(e => e && typeof e.t === 'number' && e.t >= cutoff)
      .sort((a, b) => a.t - b.t);

    const current = state.status === 'online' || state.status === 'offline' ? state.status : 'unknown';

    return json({
      now,
      since: typeof state.since === 'number' ? state.since : now,
      events,
      current: {
        state: current,
        since: current === 'unknown' || typeof state.statusSince !== 'number' ? null : state.statusSince,
      },
      ready: true,
    });
  } catch (err) {
    // Degrade gracefully: an empty-but-valid payload beats a 500 in the UI. But
    // flag ready:false so the browser can say "unavailable" instead of silently
    // rendering this as "no data yet" - that ambiguity hid a total backend
    // failure (MissingBlobsEnvironmentError) behind an innocuous placeholder.
    console.error('stats failed:', err && err.message);
    return json({
      now,
      since: now,
      events: [],
      current: { state: 'unknown', since: null },
      ready: false,
      error: err && err.message ? err.message : 'unknown error',
    });
  }
};
