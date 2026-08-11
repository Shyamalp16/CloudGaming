const { spawn } = require('child_process');
const crypto = require('crypto');
const net = require('net');
const path = require('path');
const { createClient } = require('redis');

jest.setTimeout(60000);

function getFreePort() {
  return new Promise((resolve, reject) => {
    const server = net.createServer();
    server.once('error', reject);
    server.listen(0, '127.0.0.1', () => {
      const { port } = server.address();
      server.close((error) => (error ? reject(error) : resolve(port)));
    });
  });
}

async function waitForReady(port) {
  const deadline = Date.now() + 20000;
  while (Date.now() < deadline) {
    try {
      const response = await fetch(`http://127.0.0.1:${port}/readyz`);
      if (response.status === 200) return;
    } catch (_) {}
    await new Promise((resolve) => setTimeout(resolve, 200));
  }
  throw new Error('Matchmaker did not become ready');
}

function waitForExit(child) {
  if (child.exitCode !== null || child.signalCode !== null) return Promise.resolve();
  return new Promise((resolve) => {
    const timer = setTimeout(() => {
      child.kill();
      resolve();
    }, 5000);
    child.once('exit', () => {
      clearTimeout(timer);
      resolve();
    });
  });
}

async function post(port, route, body, authorization) {
  return fetch(`http://127.0.0.1:${port}${route}`, {
    method: 'POST',
    headers: {
      'content-type': 'application/json',
      ...(authorization ? { authorization: `Bearer ${authorization}` } : {}),
    },
    body: JSON.stringify(body),
  });
}

describe('one-time pairing code rotation', () => {
  it('keeps a consumed code unavailable until the host publishes a new code', async () => {
    const redisUrl = process.env.REDIS_URL || 'redis://127.0.0.1:6379';
    const redis = createClient({
      url: redisUrl,
      socket: { connectTimeout: 1000, reconnectStrategy: false },
    });
    let child;
    const prefix = `cg:test:${crypto.randomBytes(4).toString('hex')}:`;
    try {
      try {
        await redis.connect();
      } catch (error) {
        throw new Error('Redis integration-test service is unavailable', { cause: error });
      }
      const port = await getFreePort();
      const hostId = crypto.randomUUID();
      const roomId = crypto.randomBytes(16).toString('hex');
      const firstCode = crypto.randomBytes(16).toString('hex');
      const secondCode = crypto.randomBytes(16).toString('hex');
      const hostSecret = crypto.randomBytes(32).toString('base64url');
      child = spawn(process.execPath, [path.join(__dirname, '..', 'mm_server', 'Matchmaker.js')], {
        cwd: path.join(__dirname, '..'),
        env: {
          ...process.env,
          MATCHMAKER_PORT: String(port),
          BIND_HOST: '127.0.0.1',
          REDIS_URL: redisUrl,
          REDIS_KEY_PREFIX: prefix,
          HOST_CREDENTIALS_JSON: JSON.stringify({ [hostId]: hostSecret }),
          PAIRING_TOKEN_SECRET: crypto.randomBytes(32).toString('base64url'),
          METRICS_SECRET: crypto.randomBytes(32).toString('base64url'),
          PRETTY_LOGS: 'false',
        },
        stdio: ['ignore', 'pipe', 'pipe'],
      });
      await waitForReady(port);
      const heartbeat = (pairingCode) =>
        post(
          port,
          '/api/host/heartbeat',
          { hostId, roomId, pairingCode, status: 'idle', capacity: 1, availableSlots: 1 },
          hostSecret,
        );

      expect((await heartbeat(firstCode)).status).toBe(200);
      expect((await post(port, '/api/match/find', { pairingCode: firstCode })).status).toBe(200);

      const repeated = await heartbeat(firstCode);
      expect(repeated.status).toBe(200);
      expect(await repeated.json()).toMatchObject({ rotatePairingCode: true });
      expect((await post(port, '/api/match/find', { pairingCode: firstCode })).status).toBe(404);

      const rotated = await heartbeat(secondCode);
      expect(rotated.status).toBe(200);
      expect(await rotated.json()).toMatchObject({ rotatePairingCode: false });
      expect((await post(port, '/api/match/find', { pairingCode: secondCode })).status).toBe(404);
      await redis.del(`${prefix}host_reservations:${hostId}`);
      expect((await heartbeat(secondCode)).status).toBe(200);
      expect((await post(port, '/api/match/find', { pairingCode: secondCode })).status).toBe(200);
    } finally {
      if (child) {
        child.kill('SIGTERM');
        await waitForExit(child);
      }
      if (redis.isOpen) {
        const keys = await redis.keys(`${prefix}*`);
        if (keys.length > 0) await redis.del(keys);
        await redis.quit();
      }
    }
  });
});
