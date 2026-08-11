const { spawn } = require('child_process');
const crypto = require('crypto');
const net = require('net');
const path = require('path');
const { createClient } = require('redis');
const { WebSocket } = require('ws');

jest.setTimeout(60000);

function freePort() {
  return new Promise((resolve, reject) => {
    const server = net.createServer();
    server.once('error', reject);
    server.listen(0, '127.0.0.1', () => {
      const port = server.address().port;
      server.close((error) => (error ? reject(error) : resolve(port)));
    });
  });
}

async function waitUntil(test, timeout = 20000) {
  const deadline = Date.now() + timeout;
  while (Date.now() < deadline) {
    const value = await test();
    if (value) return value;
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error('Timed out waiting for condition');
}

function request(port, route, method = 'GET', body, token) {
  return fetch(`http://127.0.0.1:${port}${route}`, {
    method,
    headers: {
      ...(body ? { 'content-type': 'application/json' } : {}),
      ...(token ? { authorization: `Bearer ${token}` } : {}),
    },
    body: body ? JSON.stringify(body) : undefined,
  });
}

function waitForExit(child) {
  if (child.exitCode !== null) return Promise.resolve();
  return new Promise((resolve) => {
    child.once('exit', resolve);
    setTimeout(resolve, 3000).unref();
  });
}

describe('unattended marketplace session', () => {
  it('advertises, allocates, prepares, bootstraps, and releases a host', async () => {
    const redisUrl = process.env.REDIS_URL || 'redis://127.0.0.1:6379';
    const redis = createClient({
      url: redisUrl,
      socket: { connectTimeout: 1000, reconnectStrategy: false },
    });
    let child;
    let signalingChild;
    let ws;
    let backupWs;
    let signalHost;
    let signalPlayer;
    const prefix = `cg:test:${crypto.randomBytes(4).toString('hex')}:`;
    try {
      try {
        await redis.connect();
      } catch (error) {
        throw new Error('Redis integration-test service is unavailable', { cause: error });
      }
      const port = await freePort();
      const hostId = crypto.randomUUID();
      const hostSecret = crypto.randomBytes(32).toString('base64url');
      const backupHostId = crypto.randomUUID();
      const backupHostSecret = crypto.randomBytes(32).toString('base64url');
      const pairingSecret = crypto.randomBytes(32).toString('base64url');
      child = spawn(process.execPath, [path.join(__dirname, '..', 'mm_server', 'Matchmaker.js')], {
        cwd: path.join(__dirname, '..'),
        env: {
          ...process.env,
          MATCHMAKER_PORT: String(port),
          BIND_HOST: '127.0.0.1',
          REDIS_URL: redisUrl,
          REDIS_KEY_PREFIX: prefix,
          HOST_CREDENTIALS_JSON: JSON.stringify({
            [hostId]: hostSecret,
            [backupHostId]: backupHostSecret,
          }),
          PAIRING_TOKEN_SECRET: pairingSecret,
        },
        stdio: ['ignore', 'pipe', 'pipe'],
      });
      await waitUntil(
        async () => (await fetch(`http://127.0.0.1:${port}/readyz`).catch(() => null))?.ok,
      );
      const signalingPort = await freePort();
      signalingChild = spawn(
        process.execPath,
        [path.join(__dirname, '..', 'ScalableSignalingServer.js')],
        {
          cwd: path.join(__dirname, '..'),
          env: {
            ...process.env,
            PORT: String(signalingPort),
            WS_PORT: String(signalingPort),
            BIND_HOST: '127.0.0.1',
            REDIS_URL: redisUrl,
            REDIS_KEY_PREFIX: prefix,
            HOST_CREDENTIALS_JSON: JSON.stringify({
              [hostId]: hostSecret,
              [backupHostId]: backupHostSecret,
            }),
            PAIRING_TOKEN_SECRET: pairingSecret,
            ENABLE_SESSION_AUTH: 'true',
            ENABLE_AUTH: 'false',
            ALLOWED_ORIGINS: 'http://localhost',
          },
          stdio: ['ignore', 'pipe', 'pipe'],
        },
      );
      await waitUntil(
        async () => (await fetch(`http://127.0.0.1:${signalingPort}/readyz`).catch(() => null))?.ok,
      );

      const messages = [];
      ws = new WebSocket(`ws://127.0.0.1:${port}/api/v1/host/control?hostId=${hostId}`, {
        headers: { authorization: `Bearer ${hostSecret}` },
      });
      ws.on('message', (raw) => messages.push(JSON.parse(raw.toString())));
      await waitUntil(() => messages.some((message) => message.type === 'control.ready'));
      const backupMessages = [];
      backupWs = new WebSocket(
        `ws://127.0.0.1:${port}/api/v1/host/control?hostId=${backupHostId}`,
        { headers: { authorization: `Bearer ${backupHostSecret}` } },
      );
      backupWs.on('message', (raw) => backupMessages.push(JSON.parse(raw.toString())));
      await waitUntil(() => backupMessages.some((message) => message.type === 'control.ready'));

      const presence = await request(
        port,
        '/api/v1/host/presence',
        'POST',
        {
          hostId,
          region: 'ca-east',
          agentVersion: '0.2.0',
          games: [
            {
              id: 'steam:730',
              source: 'steam',
              title: 'Counter-Strike 2',
              localManifestId: 'steam-730',
            },
          ],
          network: { probeRegion: 'toronto', probeRttMs: 8 },
        },
        hostSecret,
      );
      expect(presence.status).toBe(200);
      const staleGame = {
        id: 'manual:stale-game',
        source: 'manual',
        title: 'Stale game',
        localManifestId: 'manual-stale-game',
        enabled: true,
      };
      await redis.sAdd(`${prefix}market:games`, staleGame.id);
      await redis.set(`${prefix}market:game:${staleGame.id}`, JSON.stringify(staleGame));
      await redis.sAdd(`${prefix}market:idle-hosts:game:${staleGame.id}`, hostId);
      const staleCatalogResponse = await request(port, '/api/v1/games');
      const staleCatalog = await staleCatalogResponse.json();
      expect(staleCatalog.games.find((game) => game.id === staleGame.id)).toMatchObject({
        availableHosts: 0,
      });
      expect(
        Boolean(await redis.sIsMember(`${prefix}market:idle-hosts:game:${staleGame.id}`, hostId)),
      ).toBe(false);
      const backupPresence = await request(
        port,
        '/api/v1/host/presence',
        'POST',
        {
          hostId: backupHostId,
          region: 'ca-east',
          agentVersion: '0.2.0',
          games: [
            {
              id: 'steam:730',
              source: 'steam',
              title: 'Counter-Strike 2',
              localManifestId: 'steam-730-backup',
            },
          ],
          network: { probeRegion: 'toronto', probeRttMs: 40 },
        },
        backupHostSecret,
      );
      expect(backupPresence.status).toBe(200);

      const allocation = await request(port, '/api/v1/sessions', 'POST', {
        gameId: 'steam:730',
        durationSeconds: 300,
        probes: [{ region: 'toronto', rttMs: 10 }],
      });
      expect(allocation.status).toBe(202);
      const session = (await allocation.json()).session;
      expect(
        (
          await request(port, '/api/v1/sessions', 'POST', {
            gameId: 'steam:730',
            durationSeconds: 300,
          })
        ).status,
      ).toBe(409);
      const prepare = await waitUntil(() =>
        messages.find((message) => message.type === 'session.prepare'),
      );
      expect(prepare.payload.offering.localManifestId).toBe('steam-730');

      ws.send(
        JSON.stringify({
          type: 'session.failed',
          commandId: prepare.commandId,
          sessionId: session.id,
          payload: { code: 'launch_failed' },
        }),
      );
      const replacement = await waitUntil(() =>
        backupMessages.find((message) => message.type === 'session.prepare'),
      );
      expect(replacement).toMatchObject({ sessionId: session.id });
      expect(replacement.payload.offering.localManifestId).toBe('steam-730-backup');

      backupWs.send(
        JSON.stringify({
          type: 'session.launch_ack',
          commandId: replacement.commandId,
          sessionId: session.id,
          payload: {},
        }),
      );
      backupWs.send(
        JSON.stringify({ type: 'session.game_ready', sessionId: session.id, payload: {} }),
      );
      await waitUntil(async () => {
        const response = await request(port, `/api/v1/sessions/${session.id}`);
        return (await response.json()).session?.state === 'ready';
      });
      const bootstrapResponse = await request(port, `/api/v1/sessions/${session.id}/bootstrap`);
      expect(bootstrapResponse.status).toBe(200);
      const bootstrap = await bootstrapResponse.json();
      const hostSignals = [];
      const playerSignals = [];
      signalHost = new WebSocket(
        `ws://127.0.0.1:${signalingPort}`,
        [
          'cloud-gaming-v1',
          `cg-room.${bootstrap.roomId}`,
          'cg-role.host',
          `cg-host.${backupHostId}`,
        ],
        { headers: { authorization: `Bearer ${backupHostSecret}` } },
      );
      signalHost.on('message', (raw) => hostSignals.push(JSON.parse(raw.toString())));
      await waitUntil(() => signalHost.readyState === WebSocket.OPEN);
      signalPlayer = new WebSocket(
        `ws://127.0.0.1:${signalingPort}`,
        [
          'cloud-gaming-v1',
          `cg-room.${bootstrap.roomId}`,
          'cg-role.player',
          `cg-pairing.${bootstrap.pairingToken}`,
        ],
        { headers: { origin: 'http://localhost' } },
      );
      signalPlayer.on('message', (raw) => playerSignals.push(JSON.parse(raw.toString())));
      await waitUntil(() =>
        playerSignals.some(
          (message) => message.type === 'control' && message.action === 'session-ready',
        ),
      );
      signalPlayer.send(JSON.stringify({ type: 'offer', sessionId: session.id, sdp: 'v=0\r\n' }));
      expect(
        await waitUntil(() => hostSignals.find((message) => message.type === 'offer')),
      ).toMatchObject({ sessionId: session.id });

      expect((await request(port, `/api/v1/sessions/${session.id}`, 'DELETE')).status).toBe(202);
      await waitUntil(() => backupMessages.find((message) => message.type === 'session.stop'));
      backupWs.send(JSON.stringify({ type: 'session.ended', sessionId: session.id, payload: {} }));
      await waitUntil(async () => {
        const response = await request(port, `/api/v1/sessions/${session.id}`);
        return (await response.json()).session?.state === 'ended';
      });
      const catalogResponse = await request(port, '/api/v1/games');
      const catalog = await catalogResponse.json();
      expect({ status: catalogResponse.status, body: catalog }).toMatchObject({ status: 200 });
      expect(catalog.games[0]).toMatchObject({ id: 'steam:730', availableHosts: 2 });

      const secondAllocation = await request(port, '/api/v1/sessions', 'POST', {
        gameId: 'steam:730',
        durationSeconds: 300,
        probes: [{ region: 'toronto', rttMs: 10 }],
      });
      expect(secondAllocation.status).toBe(202);
      const secondSession = (await secondAllocation.json()).session;
      const secondPrepare = await waitUntil(() =>
        messages.find(
          (message) => message.type === 'session.prepare' && message.sessionId === secondSession.id,
        ),
      );
      ws.send(
        JSON.stringify({
          type: 'session.game_ready',
          commandId: secondPrepare.commandId,
          sessionId: secondSession.id,
          payload: {},
        }),
      );
      ws.send(
        JSON.stringify({
          type: 'session.stream_connected',
          sessionId: secondSession.id,
          payload: {},
        }),
      );
      await waitUntil(async () => {
        const response = await request(port, `/api/v1/sessions/${secondSession.id}`);
        return (await response.json()).session?.state === 'active';
      });
      ws.send(
        JSON.stringify({
          type: 'session.game_ready',
          sessionId: secondSession.id,
          payload: {},
        }),
      );
      await new Promise((resolve) => setTimeout(resolve, 100));
      const afterDuplicateReady = await request(port, `/api/v1/sessions/${secondSession.id}`);
      expect((await afterDuplicateReady.json()).session?.state).toBe('active');
      ws.close();
      await new Promise((resolve) => setTimeout(resolve, 100));
      const readyMessages = messages.filter((message) => message.type === 'control.ready').length;
      ws = new WebSocket(`ws://127.0.0.1:${port}/api/v1/host/control?hostId=${hostId}`, {
        headers: { authorization: `Bearer ${hostSecret}` },
      });
      ws.on('message', (raw) => messages.push(JSON.parse(raw.toString())));
      await waitUntil(
        () => messages.filter((message) => message.type === 'control.ready').length > readyMessages,
      );
      ws.send(JSON.stringify({ type: 'host.hello', payload: { state: 'Idle' } }));
      await waitUntil(async () => {
        const response = await request(port, `/api/v1/sessions/${secondSession.id}`);
        const body = await response.json();
        return body.session?.state === 'failed' && body.session.failureCode === 'host_restarted';
      });
    } finally {
      ws?.close();
      backupWs?.close();
      signalHost?.close();
      signalPlayer?.close();
      if (signalingChild) {
        signalingChild.kill('SIGTERM');
        await waitForExit(signalingChild);
      }
      if (child) {
        child.kill('SIGTERM');
        await waitForExit(child);
      }
      if (redis.isOpen) {
        const keys = await redis.keys(`${prefix}*`);
        if (keys.length) await redis.del(keys);
        await redis.quit();
      }
    }
  });
});
