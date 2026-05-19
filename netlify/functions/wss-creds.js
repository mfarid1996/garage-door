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

  return {
    statusCode: 200,
    headers: { ...headers, 'Content-Type': 'application/json' },
    body: JSON.stringify({
      url: `wss://${process.env.MQTT_HOST}:8884/mqtt`,
      username: process.env.MQTT_WSS_USER,
      password: process.env.MQTT_WSS_PASS,
      topic: 'garage/status',
    }),
  };
};
