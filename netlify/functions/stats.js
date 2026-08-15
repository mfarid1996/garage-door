const { getStore } = require('@netlify/blobs');

const STORE_NAME = 'garage-stats';
const STATE_KEY = 'state';
const RETENTION_MS = 8 * 24 * 60 * 60 * 1000;

// Read-only view of what poll-status.js has recorded. All window math (24h, 7d,
// uptime %) is done in the browser from these raw events, so the windows can be
// changed later without a redeploy or a re-flash.
exports.handler = async (event) => {
  const headers = {
    'Access-Control-Allow-Origin': '*',
    'Access-Control-Allow-Headers': 'Authorization',
    'Access-Control-Allow-Methods': 'GET, OPTIONS',
    'Cache-Control': 'no-store',
  };

  if (event.httpMethod === 'OPTIONS') return { statusCode: 200, headers, body: '' };
  if (event.httpMethod !== 'GET') return { statusCode: 405, headers, body: 'Method Not Allowed' };

  const auth = event.headers['authorization'] ?? '';
  const token = auth.startsWith('Bearer ') ? auth.slice(7) : null;
  const valid = (process.env.VALID_TOKENS ?? '').split(',').map(t => t.trim()).filter(Boolean);

  if (!token || !valid.includes(token)) {
    return { statusCode: 401, headers, body: JSON.stringify({ error: 'Unauthorized' }) };
  }

  const now = Date.now();

  try {
    // Strong consistency so a stats request made right after a poll doesn't read
    // a stale snapshot. siteID/token are injected automatically in Functions.
    const store = getStore({ name: STORE_NAME, consistency: 'strong' });
    const state = await store.get(STATE_KEY, { type: 'json' });

    if (!state || typeof state !== 'object') {
      // Poller has never written anything yet (first deploy, or blobs empty).
      return {
        statusCode: 200,
        headers: { ...headers, 'Content-Type': 'application/json' },
        body: JSON.stringify({
          now,
          since: now,
          events: [],
          current: { state: 'unknown', since: null },
        }),
      };
    }

    const cutoff = now - RETENTION_MS;
    const events = (Array.isArray(state.events) ? state.events : [])
      .filter(e => e && typeof e.t === 'number' && e.t >= cutoff)
      .sort((a, b) => a.t - b.t);

    const current = state.status === 'online' || state.status === 'offline' ? state.status : 'unknown';

    return {
      statusCode: 200,
      headers: { ...headers, 'Content-Type': 'application/json' },
      body: JSON.stringify({
        now,
        since: typeof state.since === 'number' ? state.since : now,
        events,
        current: {
          state: current,
          since: current === 'unknown' || typeof state.statusSince !== 'number' ? null : state.statusSince,
        },
      }),
    };
  } catch (err) {
    // Degrade gracefully: an empty-but-valid payload beats a 500 in the UI.
    console.error('stats failed:', err && err.message);
    return {
      statusCode: 200,
      headers: { ...headers, 'Content-Type': 'application/json' },
      body: JSON.stringify({
        now,
        since: now,
        events: [],
        current: { state: 'unknown', since: null },
        error: err && err.message ? err.message : 'unknown error',
      }),
    };
  }
};
