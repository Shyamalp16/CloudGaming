const { spawn } = require('child_process');
const crypto = require('crypto');
const net = require('net');
const path = require('path');
const { createClient } = require('redis');
const WebSocket = require('ws');
const { signPairingToken } = require('../sessionTokens');

jest.setTimeout(60000);

function getFreePort() {
  return new Promise((resolve, reject) => {
    const server = net.createServer();
    server.once('error', reject);
    server.listen(0, '127.0.0.1', () => {
      const address = server.address();
      server.close((error) =>
        error ? reject(error) : resolve(typeof address === 'object' ? address.port : 0),
      );
    });
  });
}

async function waitForReady(port, timeoutMs = 20000) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    try {
      const response = await fetch(`http://127.0.0.1:${port}/readyz`);
      if (response.status === 200) return;
    } catch (_) {}
    await new Promise((resolve) => setTimeout(resolve, 200));
  }
  throw new Error('Signaling server did not become ready');
}

function waitForExit(child, timeoutMs = 5000) {
  if (child.exitCode !== null || child.signalCode !== null) return Promise.resolve();
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error('Server did not exit')), timeoutMs);
    child.once('exit', () => {
      clearTimeout(timer);
      resolve();
    });
  });
}

function connect(url, protocols, headers = {}) {
  return new Promise((resolve, reject) => {
    const socket = new WebSocket(url, protocols, { headers });
    const timer = setTimeout(() => {
      socket.terminate();
      reject(new Error('WebSocket connection timed out'));
    }, 10000);
    socket.once('open', () => {
      clearTimeout(timer);
      resolve(socket);
    });
    socket.once('error', (error) => {
      clearTimeout(timer);
      reject(error);
    });
  });
}

function connectAndWaitForClose(url, protocols, headers = {}) {
  return new Promise((resolve, reject) => {
    const socket = new WebSocket(url, protocols, { headers });
    const timer = setTimeout(() => {
      socket.terminate();
      reject(new Error('WebSocket was not rejected'));
    }, 10000);
    socket.once('close', (code) => {
      clearTimeout(timer);
      resolve(code);
    });
    socket.once('error', () => {
      // A failed upgrade is also a safe rejection, but the server normally
      // completes the handshake and closes with the policy code.
    });
  });
}

function waitForMessage(socket, predicate, timeoutMs = 10000) {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      socket.off('message', onMessage);
      reject(new Error('Expected signaling message was not received'));
    }, timeoutMs);
    const onMessage = (raw) => {
      let message;
      try {
        message = JSON.parse(raw.toString());
      } catch (_) {
        return;
      }
      if (!predicate(message)) return;
      clearTimeout(timer);
      socket.off('message', onMessage);
      resolve(message);
    };
    socket.on('message', onMessage);
  });
}

describe('authenticated host/player session', () => {
  it('binds routing to one session and rejects pairing-token replay', async () => {
    const port = await getFreePort();
    const roomId = crypto.randomBytes(16).toString('hex');
    const hostId = crypto.randomUUID();
    const sessionId = crypto.randomUUID();
    const hostSecret = crypto.randomBytes(32).toString('base64url');
    const pairingSecret = crypto.randomBytes(32).toString('base64url');
    const prefix = `cg:test:${crypto.randomBytes(4).toString('hex')}:`;
    const redisUrl = process.env.REDIS_URL || 'redis://127.0.0.1:6379';
    const redis = createClient({
      url: redisUrl,
      socket: { connectTimeout: 1000, reconnectStrategy: false },
    });
    let child;
    let host;
    let player;

    try {
      try {
        await redis.connect();
      } catch (error) {
        throw new Error('Redis integration-test service is unavailable', { cause: error });
      }
      await redis.set(`${prefix}host-room:${hostId}`, roomId, { EX: 60 });
      const token = signPairingToken(
        { roomId, sessionId, hostId, expiresAt: Date.now() + 60000 },
        pairingSecret,
      );

      child = spawn(process.execPath, [path.join(__dirname, '..', 'ScalableSignalingServer.js')], {
        cwd: path.join(__dirname, '..'),
        env: {
          ...process.env,
          PORT: String(port),
          WS_PORT: String(port),
          BIND_HOST: '127.0.0.1',
          REDIS_URL: redisUrl,
          REDIS_KEY_PREFIX: prefix,
          ENABLE_SESSION_AUTH: 'true',
          ENABLE_AUTH: 'false',
          PAIRING_TOKEN_SECRET: pairingSecret,
          HOST_CREDENTIALS_JSON: JSON.stringify({ [hostId]: hostSecret }),
          ALLOWED_ORIGINS: 'http://localhost',
          SUBPROTOCOL: 'cloud-gaming-v1',
          PRETTY_LOGS: 'false',
          RATE_LIMIT_CONN_PER_10S: '20',
        },
        stdio: ['ignore', 'pipe', 'pipe'],
      });
      await waitForReady(port);

      const base = `ws://127.0.0.1:${port}/`;
      host = await connect(
        base,
        ['cloud-gaming-v1', `cg-room.${roomId}`, 'cg-role.host', `cg-host.${hostId}`],
        {
          Authorization: `Bearer ${hostSecret}`,
        },
      );
      player = await connect(
        base,
        ['cloud-gaming-v1', `cg-room.${roomId}`, 'cg-role.player', `cg-pairing.${token}`],
        {
          Origin: 'http://localhost',
        },
      );
      await waitForMessage(
        player,
        (message) => message.type === 'control' && message.action === 'session-ready',
      );

      const playerAnswer = waitForMessage(player, (message) => message.type === 'answer');
      host.send(JSON.stringify({ type: 'answer', sessionId, sdp: 'v=0\r\n' }));
      await expect(playerAnswer).resolves.toMatchObject({ type: 'answer', sessionId });

      const hostOffer = waitForMessage(host, (message) => message.type === 'offer');
      player.send(JSON.stringify({ type: 'offer', sessionId, sdp: 'v=0\r\n' }));
      await expect(hostOffer).resolves.toMatchObject({ type: 'offer', sessionId });

      const playerClosed = new Promise((resolve) => player.once('close', resolve));
      player.close();
      await playerClosed;
      await new Promise((resolve) => setTimeout(resolve, 150));
      const replayCode = await connectAndWaitForClose(
        base,
        ['cloud-gaming-v1', `cg-room.${roomId}`, 'cg-role.player', `cg-pairing.${token}`],
        { Origin: 'http://localhost' },
      );
      expect(replayCode).toBe(1008);
    } finally {
      if (host && host.readyState < WebSocket.CLOSING) host.close();
      if (player && player.readyState < WebSocket.CLOSING) player.close();
      if (child) {
        child.kill('SIGTERM');
        await waitForExit(child).catch(() => child.kill());
      }
      if (redis.isOpen) {
        const keys = await redis.keys(`${prefix}*`);
        if (keys.length > 0) await redis.del(keys);
        await redis.quit();
      }
    }
  });
});
