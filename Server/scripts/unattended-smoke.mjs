import { spawn } from 'node:child_process';
import { randomBytes, randomUUID } from 'node:crypto';
import { createConnection } from 'node:net';
import path from 'node:path';
import process from 'node:process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const native = path.resolve(root, '..', 'x64', 'Release', 'DisplayCaptureProject.exe');
const pipeName = `ReflexGaming.HostAgent.smoke.${process.pid}`;
const secret = randomBytes(32).toString('base64url');
const env = {
  ...process.env,
  NODE_ENV: 'development',
  BIND_HOST: '127.0.0.1',
  MATCHMAKER_PORT: '3000',
  WS_PORT: '3002',
  HEALTH_PORT: '18081',
  REDIS_URL: 'redis://127.0.0.1:6379',
  REDIS_KEY_PREFIX: `cg:smoke:${randomBytes(6).toString('hex')}:`,
  HOST_SECRET: secret,
  HOST_CREDENTIALS_JSON: '',
  CLOUDGAMING_HOST_SECRET: secret,
  PAIRING_TOKEN_SECRET: randomBytes(32).toString('base64url'),
  METRICS_SECRET: randomBytes(32).toString('base64url'),
  ENABLE_AUTH: 'false',
  ENABLE_SESSION_AUTH: 'true',
  REQUIRE_WSS: 'false',
  ALLOWED_ORIGINS: 'http://127.0.0.1:1420,http://localhost:1420',
};
const children = [];
let client;
let restoredGame;

try {
  children.push(start(process.execPath, ['ScalableSignalingServer.js'], root));
  children.push(start(process.execPath, [path.join('mm_server', 'Matchmaker.js')], root));
  await waitFor(async () => (await fetch('http://127.0.0.1:3000/readyz').catch(() => null))?.ok);
  await waitForPort(3002);

  children.push(start(native, ['--agent', '--pipe-name', pipeName], path.dirname(native)));
  client = protocolClient(await connectPipe(pipeName));
  await client.request('host.start');
  const ready = await waitFor(async () => {
    const snapshot = await client.request('host.getSnapshot');
    if (snapshot.status.state === 'Failed') throw new Error(snapshot.status.failureReason);
    return snapshot.status.state === 'Idle' ? snapshot : null;
  }, 30000);

  const games = await client.request('inventory.list');
  const installed = games.find((game) => game.installed);
  if (installed) {
    restoredGame = installed;
    if (!installed.enabled)
      await client.request('inventory.setEnabled', { id: installed.id, enabled: true });
    await waitFor(async () => {
      const response = await fetch('http://127.0.0.1:3000/api/v1/games');
      const catalog = await response.json();
      return catalog.games.some((game) => game.id === installed.id && game.availableHosts === 1);
    });
  }
  const ping = await (await fetch('http://127.0.0.1:3000/api/v1/ping')).json();
  if (ping.region !== 'local') throw new Error('Unexpected local probe region');
  console.log(
    JSON.stringify({
      state: ready.status.state,
      inventoryGames: games.length,
      catalogVerified: Boolean(installed),
    }),
  );
} finally {
  if (client) {
    if (restoredGame && !restoredGame.enabled) {
      await client
        .request('inventory.setEnabled', { id: restoredGame.id, enabled: false })
        .catch(() => undefined);
    }
    await client.request('host.stop').catch(() => undefined);
    await waitFor(async () => {
      const snapshot = await client.request('host.getSnapshot').catch(() => null);
      return snapshot?.status.state === 'Stopped';
    }, 15000).catch(() => undefined);
    await client.request('host.shutdownAgent').catch(() => undefined);
    client.close();
  }
  for (const child of children.reverse()) {
    if (child.exitCode === null) child.kill();
  }
}

function start(command, args, cwd) {
  const child = spawn(command, args, {
    cwd,
    env,
    windowsHide: true,
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  let output = '';
  child.stdout.on('data', (data) => {
    output = (output + data).slice(-8000);
  });
  child.stderr.on('data', (data) => {
    output = (output + data).slice(-8000);
  });
  child.on('exit', (code) => {
    if (code && !process.exitCode) console.error(output);
  });
  return child;
}

async function waitFor(check, timeout = 15000) {
  const deadline = Date.now() + timeout;
  let lastError;
  while (Date.now() < deadline) {
    try {
      const result = await check();
      if (result) return result;
    } catch (error) {
      lastError = error;
    }
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw lastError || new Error('Timed out');
}

function waitForPort(port) {
  return waitFor(
    () =>
      new Promise((resolve) => {
        const socket = createConnection(port, '127.0.0.1');
        socket.once('connect', () => {
          socket.destroy();
          resolve(true);
        });
        socket.once('error', () => resolve(false));
      }),
  );
}

function connectPipe(name) {
  return waitFor(
    () =>
      new Promise((resolve) => {
        const socket = createConnection(`\\\\.\\pipe\\${name}`);
        socket.once('connect', () => resolve(socket));
        socket.once('error', () => resolve(null));
      }),
  );
}

function protocolClient(socket) {
  let buffer = Buffer.alloc(0);
  const pending = new Map();
  socket.on('data', (chunk) => {
    buffer = Buffer.concat([buffer, chunk]);
    while (buffer.length >= 4) {
      const length = buffer.readUInt32LE(0);
      if (buffer.length < length + 4) return;
      const message = JSON.parse(buffer.subarray(4, length + 4).toString());
      buffer = buffer.subarray(length + 4);
      const handler = pending.get(message.requestId);
      if (!handler) continue;
      pending.delete(message.requestId);
      message.ok
        ? handler.resolve(message.result)
        : handler.reject(new Error(message.error?.message || 'Native request failed'));
    }
  });
  return {
    request(method, params = {}) {
      const requestId = randomUUID();
      const body = Buffer.from(
        JSON.stringify({ protocolVersion: 1, kind: 'request', requestId, method, params }),
      );
      const frame = Buffer.allocUnsafe(body.length + 4);
      frame.writeUInt32LE(body.length);
      body.copy(frame, 4);
      return new Promise((resolve, reject) => {
        pending.set(requestId, { resolve, reject });
        socket.write(frame);
      });
    },
    close: () => socket.destroy(),
  };
}
