const mqtt = require('mqtt');

exports.handler = async (event) => {
  const headers = {
    'Access-Control-Allow-Origin': '*',
    'Access-Control-Allow-Headers': 'Authorization',
    'Access-Control-Allow-Methods': 'POST, OPTIONS',
  };

  if (event.httpMethod === 'OPTIONS') return { statusCode: 200, headers, body: '' };
  if (event.httpMethod !== 'POST') return { statusCode: 405, headers, body: 'Method Not Allowed' };

  const auth = event.headers['authorization'] ?? '';
  const token = auth.startsWith('Bearer ') ? auth.slice(7) : null;
  const valid = (process.env.VALID_TOKENS ?? '').split(',').map(t => t.trim()).filter(Boolean);

  if (!token || !valid.includes(token)) {
    return { statusCode: 401, headers, body: JSON.stringify({ error: 'Unauthorized' }) };
  }

  const client = mqtt.connect(`mqtts://${process.env.MQTT_HOST}`, {
    port: 8883,
    username: process.env.MQTT_USER,
    password: process.env.MQTT_PASS,
    rejectUnauthorized: false,
  });

  return new Promise((resolve) => {
    const bail = (code, msg) => { client.end(true); resolve({ statusCode: code, headers, body: JSON.stringify({ error: msg }) }); };
    const timer = setTimeout(() => bail(504, 'Timeout'), 8000);

    client.on('connect', () => {
      client.publish('garage/trigger', '1', { qos: 1 }, (err) => {
        clearTimeout(timer);
        client.end();
        resolve({ statusCode: err ? 500 : 200, headers, body: JSON.stringify(err ? { error: err.message } : { ok: true }) });
      });
    });

    client.on('error', (err) => { clearTimeout(timer); bail(500, err.message); });
  });
};
