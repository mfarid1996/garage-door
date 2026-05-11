const mqtt = require('mqtt');

const ACK_TIMEOUT_MS = 4000;
const CONNECT_TIMEOUT_MS = 6000;

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
    let settled = false;
    const settle = (statusCode, body) => {
      if (settled) return;
      settled = true;
      client.end(true);
      resolve({ statusCode, headers, body: JSON.stringify(body) });
    };

    const connectTimer = setTimeout(() => settle(504, { error: 'Connect timeout' }), CONNECT_TIMEOUT_MS);

    client.on('connect', () => {
      clearTimeout(connectTimer);
      client.subscribe('garage/ack', { qos: 1 }, (subErr) => {
        if (subErr) return settle(500, { error: subErr.message });
        client.publish('garage/trigger', '1', { qos: 1 }, (pubErr) => {
          if (pubErr) return settle(500, { error: pubErr.message });
          setTimeout(() => settle(200, { ok: true, confirmed: false, reason: 'timeout' }), ACK_TIMEOUT_MS);
        });
      });
    });

    client.on('message', (topic) => {
      if (topic === 'garage/ack') settle(200, { ok: true, confirmed: true });
    });

    client.on('error', (err) => settle(500, { error: err.message }));
  });
};
