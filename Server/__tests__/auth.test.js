const { spawn } = require('child_process');
const path = require('path');
const net = require('net');
const WebSocket = require('ws');
const jwt = require('jsonwebtoken');

function getFreePort() {
  return new Promise((resolve, reject) => {
    const srv = net.createServer();
    srv.listen(0, '127.0.0.1', () => {
      const addr = srv.address();
      const port = typeof addr === 'object' && addr ? addr.port : undefined;
      srv.close((err) => (err ? reject(err) : resolve(port)));
    });
    srv.on('error', reject);
  });
}

jest.setTimeout(60000);

function redisIsAvailable() {
  return new Promise((resolve) => {
    const socket = net.createConnection({ host: '127.0.0.1', port: 6379 });
    const done = (value) => {
      socket.destroy();
      resolve(value);
    };
    socket.setTimeout(1000, () => done(false));
    socket.once('connect', () => done(true));
    socket.once('error', () => done(false));
  });
}

function waitForExit(child, timeoutMs = 15000) {
  if (child.exitCode !== null || child.signalCode !== null)
    return Promise.resolve({ code: child.exitCode, signal: child.signalCode });
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error('Child did not exit in time')), timeoutMs);
    child.once('exit', (code, signal) => {
      clearTimeout(timer);
      resolve({ code, signal });
    });
  });
}

async function waitForReady(healthPort, timeoutMs = 20000) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    try {
      const res = await fetch(`http://127.0.0.1:${healthPort}/readyz`);
      if (res.status === 200) return true;
    } catch (_) {}
    await new Promise((r) => setTimeout(r, 250));
  }
  throw new Error('Server not ready in time');
}

function startServer({ wsPort, healthPort }) {
  const serverPath = path.join(__dirname, '..', 'ScalableSignalingServer.js');
  const child = spawn(process.execPath, [serverPath], {
    env: {
      ...process.env,
      PORT: String(wsPort),
      WS_PORT: String(wsPort),
      HEALTH_PORT: String(healthPort),
      REDIS_URL: process.env.REDIS_URL || 'redis://127.0.0.1:6379',
      PRETTY_LOGS: 'false',
      ENABLE_SESSION_AUTH: 'false',
      ALLOWED_ORIGINS: 'http://localhost',
      SUBPROTOCOL: 'cloud-gaming-v1',
      ENABLE_AUTH: 'true',
      JWT_ALG: 'HS256',
      JWT_SECRET: 'test-secret-that-is-at-least-32-bytes',
      JWT_ISSUER: 'http://localhost',
      JWT_AUDIENCE: 'test',
    },
    cwd: path.join(__dirname, '..'),
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  return child;
}

function connect(url, accessToken, roomId) {
  return new Promise((resolve, reject) => {
    const protocols = [
      'cloud-gaming-v1',
      `cg-room.${roomId}`,
      'cg-role.player',
      `cg-access.${accessToken}`,
    ];
    const ws = new WebSocket(url, protocols, { headers: { origin: 'http://localhost' } });
    ws.once('open', () => resolve(ws));
    ws.once('error', reject);
  });
}

describe('JWT auth and room authorization', () => {
  it('accepts authorized token and rejects unauthorized room', async () => {
    if (!(await redisIsAvailable())) {
      if (process.env.CI) throw new Error('Redis integration-test service is unavailable');
      console.warn('Skipping Redis-backed integration assertion: local Redis is unavailable');
      return;
    }
    const wsPort = await getFreePort();
    const healthPort = await getFreePort();
    const child = startServer({ wsPort, healthPort });
    try {
      await waitForReady(wsPort, 20000);

      const allowedRoom = '11111111111111111111111111111111';
      const forbiddenRoom = '22222222222222222222222222222222';
      const token = jwt.sign(
        { sub: 'u1', role: 'player', iss: 'http://localhost', aud: 'test', rooms: [allowedRoom] },
        'test-secret-that-is-at-least-32-bytes',
        { algorithm: 'HS256', expiresIn: '5m' },
      );

      // Allowed room
      const signalingUrl = `ws://127.0.0.1:${wsPort}/`;
      const wsOk = await connect(signalingUrl, token, allowedRoom);
      expect(wsOk.readyState).toBe(WebSocket.OPEN);
      wsOk.close();

      // Forbidden room: should close quickly with 1008
      const wsBad = await connect(signalingUrl, token, forbiddenRoom);
      const closeCode = await new Promise((resolve, reject) => {
        const t = setTimeout(() => reject(new Error('forbidden ws not closed')), 5000);
        wsBad.once('close', (code) => {
          clearTimeout(t);
          resolve(code);
        });
      });
      expect([1008, 1000]).toContain(closeCode);
    } finally {
      try {
        child.kill('SIGTERM');
      } catch (_) {}
      try {
        await waitForExit(child, 5000);
      } catch (_) {}
    }
  });
});
