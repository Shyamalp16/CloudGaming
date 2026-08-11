const express = require('express');
const { randomUUID } = require('crypto');
const { signPairingToken } = require('../sessionTokens');
const { config } = require('../config');
const { createClient } = require('redis');
const { z } = require('zod');
const https = require('https');
const { RateLimiter } = require('../rateLimiter');
const { HostControlGateway } = require('../controlGateway');
const { playerId: authenticatedPlayerId } = require('../playerAuth');
const marketMetrics = require('../marketMetrics');
const {
  CLAIM_SCRIPT,
  PRESENCE_SCRIPT,
  PresenceSchema,
  RELEASE_SCRIPT,
  SessionRequestSchema,
  hostScore,
  newSession,
} = require('../orchestration');
const {
  bearerToken,
  hostCredentialValid,
  isTrustedProxy,
  originAllowed,
  secureEqual,
  sha256,
} = require('../security');

// Catch anything that would silently kill the process and log it first.
let fatalExitScheduled = false;
function fatalExit(kind, error) {
  console.error({ level: 'fatal', message: kind, error: safeError(error) });
  if (fatalExitScheduled) return;
  fatalExitScheduled = true;
  process.exitCode = 1;
  setTimeout(() => process.exit(1), 1000).unref();
}
process.on('uncaughtException', (err) => fatalExit('uncaughtException', err));
process.on('unhandledRejection', (reason) => fatalExit('unhandledRejection', reason));

function safeError(error) {
  if (!error || typeof error !== 'object') return { name: 'Error' };
  return {
    name: typeof error.name === 'string' ? error.name.slice(0, 64) : 'Error',
    code: typeof error.code === 'string' ? error.code.slice(0, 64) : undefined,
  };
}

// ─── Metered TURN helper ─────────────────────────────────────────────────────
// Fetches short-lived ICE server credentials from the Metered API.
// Cached briefly so simultaneous matches do not trigger duplicate external API calls.
// Production fails closed if TURN is unavailable; local development can use
// direct host candidates without contacting a third-party STUN service.
let iceServerCache = null;
let iceServerCacheExpiresAt = 0;
let iceServerRequest = null;
const IceServerSchema = z
  .object({
    urls: z.union([
      z.string().min(1).max(2048),
      z.array(z.string().min(1).max(2048)).min(1).max(8),
    ]),
    username: z.string().max(1024).optional(),
    credential: z.string().max(4096).optional(),
    credentialType: z.literal('password').optional(),
  })
  .strict()
  .refine((server) => {
    const urls = Array.isArray(server.urls) ? server.urls : [server.urls];
    return urls.every((value) => /^(?:stun|stuns|turn|turns):[^\s]+$/i.test(value));
  });

async function getIceServers() {
  const { domain, apiKey, expirySeconds } = config.metered;
  if (!domain || !apiKey) {
    if (config.env === 'production') throw new Error('TURN service is not configured');
    return [];
  }
  if (iceServerCache && Date.now() < iceServerCacheExpiresAt) return iceServerCache;
  if (iceServerRequest) return iceServerRequest;

  iceServerRequest = (async () => {
    try {
      // Step 1: create an expiring credential
      const createRes = await fetchJson(
        `https://${domain}.metered.live/api/v1/turn/credential?secretKey=${encodeURIComponent(apiKey)}`,
        'POST',
        { expiryInSeconds: expirySeconds },
      );
      if (!createRes || !createRes.apiKey) {
        log('warn', 'Metered credential creation returned no apiKey');
        throw new Error('TURN credential response was incomplete');
      }

      // Step 2: fetch the full iceServers array using the one-time apiKey
      const iceServers = await fetchJson(
        `https://${domain}.metered.live/api/v1/turn/credentials?apiKey=${encodeURIComponent(createRes.apiKey)}`,
        'GET',
      );
      if (!Array.isArray(iceServers) || iceServers.length === 0 || iceServers.length > 8) {
        log('warn', 'Metered returned empty iceServers');
        throw new Error('TURN service returned an invalid server list');
      }
      const validated = iceServers.map((server) => IceServerSchema.parse(server));

      iceServerCache = validated;
      const cacheSeconds = Math.max(30, Math.min(600, expirySeconds - 60));
      iceServerCacheExpiresAt = Date.now() + cacheSeconds * 1000;
      return iceServerCache;
    } catch (err) {
      log('error', 'Failed to fetch Metered TURN credentials', { error: safeError(err) });
      if (config.env === 'production') throw err;
      return [];
    }
  })();
  try {
    return await iceServerRequest;
  } finally {
    iceServerRequest = null;
  }
}

function fetchJson(url, method, body) {
  return new Promise((resolve, reject) => {
    const parsed = new URL(url);
    const data = body ? JSON.stringify(body) : null;
    const options = {
      hostname: parsed.hostname,
      path: parsed.pathname + parsed.search,
      method: method || 'GET',
      headers: {
        'Content-Type': 'application/json',
        ...(data ? { 'Content-Length': Buffer.byteLength(data) } : {}),
      },
    };
    const req = https.request(options, (res) => {
      let raw = '';
      res.on('data', (chunk) => {
        raw += chunk;
        if (raw.length > 1024 * 1024) req.destroy(new Error('Response too large'));
      });
      res.on('end', () => {
        if (res.statusCode < 200 || res.statusCode >= 300) {
          reject(new Error(`TURN provider returned HTTP ${res.statusCode}`));
          return;
        }
        try {
          resolve(JSON.parse(raw));
        } catch (e) {
          reject(new Error('TURN provider returned invalid JSON'));
        }
      });
    });
    req.on('error', reject);
    req.setTimeout(5000, () => req.destroy(new Error('Request timed out')));
    if (data) req.write(data);
    req.end();
  });
}
// ─────────────────────────────────────────────────────────────────────────────

const app = express();
app.set('trust proxy', (address) => isTrustedProxy(address, config.trustedProxyIps));
const redisKey = (name) => `${config.redisPrefix}${name}`;

// Hard-wire CORS headers on every response.  This MUST be the very first
// middleware so that OPTIONS preflights are answered before anything else
// (including any error paths) can run.
app.use((req, res, next) => {
  res.setHeader('Cache-Control', 'no-store');
  res.setHeader('X-Content-Type-Options', 'nosniff');
  res.setHeader('Referrer-Policy', 'no-referrer');
  const origin = req.headers.origin;
  if (origin) {
    if (!originAllowed(origin, config.allowedOrigins)) {
      return res.status(403).json({ success: false, error: 'Origin not allowed' });
    }
    res.setHeader('Access-Control-Allow-Origin', origin);
    res.setHeader('Vary', 'Origin');
  }
  res.setHeader('Access-Control-Allow-Methods', 'GET, POST, DELETE, OPTIONS');
  res.setHeader('Access-Control-Allow-Headers', 'Content-Type, Authorization, X-Request-Id');
  res.setHeader('Access-Control-Max-Age', '600');
  if (req.method === 'OPTIONS') return res.sendStatus(204);
  next();
});

app.use('/api/', async (req, res, next) => {
  const credential = bearerToken(req.headers);
  const id = credential
    ? `credential:${sha256(credential).slice(0, 24)}`
    : req.ip || (req.socket && req.socket.remoteAddress) || 'unknown';
  let limit = 30;
  if (req.path === '/host/heartbeat' || req.path === '/v1/host/presence') limit = 180;
  else if (req.method === 'GET') limit = 180;
  else if (req.path === '/v1/sessions') limit = 20;
  const allowed = await apiRateLimiter.allow({ namespace: 'http', id, limit, periodSeconds: 60 });
  if (!allowed) return res.status(429).json({ success: false, error: 'Rate limited' });
  next();
});

// Parse JSON request bodies for heartbeat and match APIs.
app.use(express.json({ limit: '16kb', strict: true, type: 'application/json' }));

// ── Health / readiness probes ─────────────────────────────────────────────────
// Railway (and other platforms) hit these before routing real traffic.
// Respond immediately so the container is never killed for a missing probe.
app.get('/healthz', (_req, res) => res.sendStatus(200));
app.get('/metrics', async (req, res) => {
  if (config.metricsSecret && !secureEqual(bearerToken(req.headers), config.metricsSecret))
    return res.status(401).send('Unauthorized');
  const result = await marketMetrics.metrics();
  res.type(result.contentType).send(result.body);
});
app.get('/readyz', (_req, res) => res.sendStatus(redisClient.isReady ? 200 : 503));
app.get('/health', (_req, res) => res.sendStatus(200));
app.get('/', (_, res) => res.send('ok'));
// ─────────────────────────────────────────────────────────────────────────────

function log(level, message, meta) {
  const safe = { ...(meta || {}) };
  for (const key of ['hostId', 'roomId', 'sessionId', 'pairingToken']) {
    if (safe[key] !== undefined) safe[key] = '[REDACTED]';
  }
  const entry = { level, message, ...safe };
  if (level === 'error') {
    console.error(entry);
  } else if (level === 'warn') {
    console.warn(entry);
  } else {
    console.log(entry);
  }
}

app.use((req, res, next) => {
  const headerId = req.headers['x-request-id'];
  const supplied = typeof headerId === 'string' ? headerId.trim() : '';
  const reqId = /^[A-Za-z0-9._-]{1,64}$/.test(supplied) ? supplied : randomUUID();
  req.id = reqId;
  res.setHeader('x-request-id', reqId);
  next();
});

function formatZodIssues(zodError) {
  return zodError.errors.map((e) => ({
    path: e.path.join('.'),
    message: e.message,
    code: e.code,
  }));
}

const authenticateHost = (req, res, next) => {
  const credential = bearerToken(req.headers);
  const hostId = req.body && req.body.hostId;
  if (!credential) {
    return res.status(401).json({
      success: false,
      error: 'Unauthorized: Missing or invalid Authorization header',
    });
  }
  if (!hostCredentialValid(config, hostId, credential)) {
    return res.status(403).json({
      success: false,
      error: 'Forbidden: Invalid host secret',
    });
  }

  next();
};

app.use('/api/host', authenticateHost);
app.use('/api/v1/host', authenticateHost);

const HeartbeatSchema = z
  .object({
    hostId: z.string().uuid(),
    roomId: z.string().regex(/^[a-f0-9]{32}$/i),
    pairingCode: z.string().regex(/^[a-f0-9]{32}$/i),
    region: z
      .string()
      .trim()
      .min(1)
      .max(32)
      .regex(/^[A-Za-z0-9_-]+$/)
      .optional(),
    status: z.enum(['idle', 'busy', 'allocated']).optional(),
    capacity: z.number().int().min(1).max(4).optional(),
    availableSlots: z.number().int().min(0).max(4).optional(),
  })
  .strict();

const MatchFindSchema = z
  .object({
    pairingCode: z
      .string()
      .trim()
      .regex(/^[a-f0-9]{32}$/i),
  })
  .strict();

const HEARTBEAT_SCRIPT = `
local host = cjson.decode(ARGV[1])
local capacity = tonumber(host.capacity) or 1
local available = tonumber(host.availableSlots) or capacity
local reserved = tonumber(redis.call('GET', KEYS[3]) or '0')
available = math.max(0, math.min(capacity, available - reserved))
host.availableSlots = available
host.status = available > 0 and 'idle' or 'busy'
redis.call('SET', KEYS[1], cjson.encode(host), 'EX', ARGV[3])
local pairingConsumed = redis.call('GET', KEYS[6]) == ARGV[4]
if available > 0 and not pairingConsumed then
  redis.call('SADD', KEYS[2], ARGV[2])
  redis.call('SET', KEYS[4], ARGV[2], 'EX', ARGV[3])
else
  redis.call('SREM', KEYS[2], ARGV[2])
  redis.call('DEL', KEYS[4])
end
if not pairingConsumed and redis.call('EXISTS', KEYS[6]) == 1 then
  redis.call('DEL', KEYS[6])
end
redis.call('SET', KEYS[5], host.roomId, 'EX', ARGV[3])
return {available, pairingConsumed and 1 or 0}
`;

const CLAIM_HOST_SCRIPT = `
local raw = redis.call('GET', KEYS[1])
if not raw then
  redis.call('SREM', KEYS[2], ARGV[1])
  return nil
end
local host = cjson.decode(raw)
local capacity = tonumber(host.capacity) or 1
local available = tonumber(host.availableSlots) or capacity
if available <= 0 then
  redis.call('SREM', KEYS[2], ARGV[1])
  return nil
end
available = available - 1
host.capacity = capacity
host.availableSlots = available
host.status = available > 0 and 'idle' or 'busy'
redis.call('INCR', KEYS[3])
redis.call('EXPIRE', KEYS[3], ARGV[3])
redis.call('SET', KEYS[1], cjson.encode(host), 'EX', ARGV[2])
redis.call('DEL', KEYS[4])
redis.call('SET', KEYS[5], ARGV[4], 'EX', ARGV[2])
if available > 0 then
  redis.call('SADD', KEYS[2], ARGV[1])
else
  redis.call('SREM', KEYS[2], ARGV[1])
end
return cjson.encode(host)
`;

async function pruneStaleIdleHosts() {
  try {
    const ids = await redisClient.sMembers(redisKey('idle_hosts'));
    if (ids.length === 0) return;
    const multi = redisClient.multi();
    ids.forEach((id) => multi.ttl(redisKey(`host:${id}`)));
    const ttls = await multi.exec();
    const stale = ids.filter((_, index) => ttls[index] === -2);
    if (stale.length > 0) {
      await redisClient.sRem(redisKey('idle_hosts'), stale);
      log('info', 'Pruned stale idle hosts', { staleCount: stale.length });
    }
  } catch (error) {
    log('error', 'Failed to prune stale hosts', { error: safeError(error) });
  }
}

app.post('/api/host/heartbeat', async (req, res) => {
  const result = HeartbeatSchema.safeParse(req.body);
  if (!result.success) {
    return res.status(400).json({
      success: false,
      error: 'Validation failed',
      issues: formatZodIssues(result.error),
    });
  }
  const hostId = result.data.hostId.toLowerCase();
  const roomId = result.data.roomId.toLowerCase();
  const pairingCode = result.data.pairingCode.toLowerCase();
  const { region, status } = result.data;
  const capacity = Math.max(1, result.data.capacity || 1);
  let availableSlots = result.data.availableSlots;
  if (availableSlots === undefined || availableSlots === null) {
    availableSlots = capacity;
  }
  availableSlots = Math.max(0, Math.min(capacity, availableSlots));
  if (!hostId || !roomId) {
    return res.status(400).json({ success: false, error: 'Missing hostId or roomId' });
  }
  const key = redisKey(`host:${hostId}`);
  const host = {
    hostId,
    roomId,
    region: region || 'local',
    status: availableSlots > 0 ? status || 'idle' : status || 'busy',
    capacity,
    availableSlots,
    lastHeartbeat: Date.now(),
  };

  try {
    const pairingHash = sha256(pairingCode);
    const heartbeatResult = await redisClient.eval(HEARTBEAT_SCRIPT, {
      keys: [
        key,
        redisKey('idle_hosts'),
        redisKey(`host_reservations:${hostId}`),
        redisKey(`pairing:${pairingHash}`),
        redisKey(`host-room:${hostId}`),
        redisKey(`pairing-consumed:${hostId}`),
      ],
      arguments: [JSON.stringify(host), hostId, String(config.hostTtlSeconds), pairingHash],
    });
    log('info', 'Heartbeat accepted', { requestId: req.id, hostId, status: status || 'idle' });
    res.json({
      success: true,
      ttl: config.hostTtlSeconds,
      rotatePairingCode: Array.isArray(heartbeatResult) && Number(heartbeatResult[1]) === 1,
    });
  } catch (err) {
    log('error', 'Failed to set heartbeat', { requestId: req.id, error: safeError(err) });
    res.status(500).json({ success: false, error: 'Failed to set heartbeat' });
  }
});

const marketHostKey = (hostId) => redisKey(`market:host:${hostId}`);
const hostLeaseKey = (hostId) => redisKey(`market:host-lease:${hostId}`);
const sessionKey = (sessionId) => redisKey(`market:session:${sessionId}`);
const playerSessionKey = (playerId) => redisKey(`market:player-session:${playerId}`);
const gameHostsPrefix = () => redisKey('market:idle-hosts:game:');
const sessionTtl = (seconds) => Math.max(86400, seconds + 3600);

async function saveSession(session) {
  session.updatedAt = Date.now();
  await redisClient.set(sessionKey(session.id), JSON.stringify(session), {
    EX: sessionTtl(session.durationSeconds),
  });
  await redisClient.rPush(
    redisKey(`market:session-events:${session.id}`),
    JSON.stringify({ state: session.state, at: session.updatedAt }),
  );
  await redisClient.expire(
    redisKey(`market:session-events:${session.id}`),
    sessionTtl(session.durationSeconds),
  );
  return session;
}

async function loadSession(sessionId) {
  const raw = await redisClient.get(sessionKey(sessionId));
  return raw ? JSON.parse(raw) : null;
}

async function releaseHost(hostId, sessionId) {
  return redisClient.eval(RELEASE_SCRIPT, {
    keys: [marketHostKey(hostId), hostLeaseKey(hostId)],
    arguments: [gameHostsPrefix(), sessionId, hostId, String(config.hostTtlSeconds)],
  });
}

async function releasePlayer(session) {
  const key = playerSessionKey(session.playerId);
  if ((await redisClient.get(key)) === session.id) await redisClient.del(key);
}

async function allocateSession(session) {
  const ids = await redisClient.sMembers(`${gameHostsPrefix()}${session.gameId}`);
  const attempted = new Set(session.attemptedHostIds || []);
  const eligibleIds = ids.filter((id) => !attempted.has(id));
  if (!eligibleIds.length) return null;
  const multi = redisClient.multi();
  eligibleIds.forEach((id) => multi.get(marketHostKey(id)));
  const candidates = (await multi.exec())
    .map((raw) => (raw ? JSON.parse(raw) : null))
    .filter((host) => host && controlGateway.connected(host.hostId))
    .sort((a, b) => hostScore(a, session.probes || []) - hostScore(b, session.probes || []));

  for (const host of candidates) {
    attempted.add(host.hostId);
    const allocated = {
      ...session,
      hostId: host.hostId,
      state: 'reserved',
      attemptedHostIds: [...attempted],
    };
    const claimed = await redisClient.eval(CLAIM_SCRIPT, {
      keys: [marketHostKey(host.hostId), hostLeaseKey(host.hostId), sessionKey(session.id)],
      arguments: [
        gameHostsPrefix(),
        session.gameId,
        session.id,
        host.hostId,
        String(session.durationSeconds + 300),
        String(config.hostTtlSeconds),
        JSON.stringify(allocated),
        String(sessionTtl(session.durationSeconds)),
      ],
    });
    if (!claimed) continue;
    const offering = host.games.find((game) => game.id === session.gameId);
    const prepareDeadlineAt = Date.now() + 180000;
    const commandId = controlGateway.send(host.hostId, 'session.prepare', session.id, {
      offering,
      roomId: session.roomId,
      durationSeconds: session.durationSeconds,
      prepareDeadline: prepareDeadlineAt,
      streamProfile: session.streamProfile,
    });
    if (!commandId) {
      await releaseHost(host.hostId, session.id);
      continue;
    }
    allocated.state = 'preparing';
    allocated.commandId = commandId;
    allocated.prepareDeadlineAt = prepareDeadlineAt;
    await saveSession(allocated);
    await redisClient.zAdd(redisKey('market:session-prepare-deadlines'), {
      score: prepareDeadlineAt,
      value: session.id,
    });
    return allocated;
  }
  return null;
}

async function failOrRetrySession(session, code) {
  const active = ['active', 'ending'].includes(session.state);
  await releaseHost(session.hostId, session.id);
  await redisClient.zRem(redisKey('market:session-prepare-deadlines'), session.id);
  await redisClient.zRem(redisKey('market:session-deadlines'), session.id);
  if (!active) {
    session.state = 'allocating';
    session.failureCode = code;
    const replacement = await allocateSession(session);
    if (replacement) {
      marketMetrics.sessionFailover(code);
      return replacement;
    }
  }
  session.state = 'failed';
  session.failureCode = code;
  session.endedAt = Date.now();
  await releasePlayer(session);
  marketMetrics.sessionOutcome('failed');
  return saveSession(session);
}

async function handleControlConnection(hostId, connected) {
  const key = redisKey('market:host-disconnect-deadlines');
  if (connected) await redisClient.zRem(key, hostId);
  else
    await redisClient.zAdd(key, {
      score: Date.now() + 30000,
      value: hostId,
    });
}

async function handleHostEvent(hostId, event) {
  if (event.type === 'host.hello') {
    const leasedSessionId = await redisClient.get(hostLeaseKey(hostId));
    if (event.sessionId && event.sessionId !== leasedSessionId) {
      controlGateway.send(hostId, 'session.stop', event.sessionId, { reason: 'orphaned_session' });
    } else if (!event.sessionId && leasedSessionId) {
      const leasedSession = await loadSession(leasedSessionId);
      if (leasedSession && !['ended', 'failed'].includes(leasedSession.state))
        await failOrRetrySession(leasedSession, 'host_restarted');
    }
    return;
  }
  if (!event.sessionId) return;
  const session = await loadSession(event.sessionId);
  if (!session || session.hostId !== hostId || ['ended', 'failed'].includes(session.state)) return;
  if (event.type === 'session.launch_ack') session.state = 'preparing';
  if (event.type === 'session.game_ready') {
    session.state = 'ready';
    marketMetrics.observeReady(Math.max(0, (Date.now() - session.createdAt) / 1000));
    await redisClient.zRem(redisKey('market:session-prepare-deadlines'), session.id);
  }
  if (event.type === 'session.stream_connected') {
    session.state = 'active';
    session.startsAt = Date.now();
    session.endsAt = session.startsAt + session.durationSeconds * 1000;
    await redisClient.zAdd(redisKey('market:session-deadlines'), {
      score: session.endsAt,
      value: session.id,
    });
  }
  if (event.type === 'session.failed') {
    await failOrRetrySession(session, event.payload.code || 'host_failed');
    return;
  }
  if (event.type === 'session.ended') {
    session.state = 'ended';
    session.endedAt = Date.now();
    await releaseHost(hostId, session.id);
    await redisClient.zRem(redisKey('market:session-prepare-deadlines'), session.id);
    await redisClient.zRem(redisKey('market:session-deadlines'), session.id);
    await releasePlayer(session);
    marketMetrics.sessionOutcome('ended');
  }
  await saveSession(session);
}

app.post('/api/v1/host/presence', async (req, res) => {
  const parsed = PresenceSchema.safeParse(req.body);
  if (!parsed.success) {
    return res.status(400).json({
      success: false,
      error: 'Validation failed',
      issues: formatZodIssues(parsed.error),
    });
  }
  const presence = parsed.data;
  try {
    const multi = redisClient.multi();
    for (const game of presence.games) {
      multi.sAdd(redisKey('market:games'), game.id);
      multi.set(redisKey(`market:game:${game.id}`), JSON.stringify(game));
    }
    await multi.exec();
    const state = await redisClient.eval(PRESENCE_SCRIPT, {
      keys: [marketHostKey(presence.hostId), hostLeaseKey(presence.hostId)],
      arguments: [
        presence.hostId,
        JSON.stringify(presence),
        gameHostsPrefix(),
        String(Date.now()),
        String(config.hostTtlSeconds),
      ],
    });
    res.json({ success: true, state, controlConnected: controlGateway.connected(presence.hostId) });
  } catch (error) {
    log('error', 'Host presence update failed', { error: safeError(error) });
    res.status(503).json({ success: false, error: 'Presence service unavailable' });
  }
});

app.get('/api/v1/games', async (_req, res) => {
  try {
    const ids = await redisClient.sMembers(redisKey('market:games'));
    if (!ids.length) return res.json({ games: [] });
    const memberships = await Promise.all(
      ids.map((id) => redisClient.sMembers(`${gameHostsPrefix()}${id}`)),
    );
    const hostIds = [...new Set(memberships.flat())];
    const presence = new Map();
    if (hostIds.length) {
      const hosts = await redisClient.mGet(hostIds.map(marketHostKey));
      hostIds.forEach((hostId, index) => {
        if (hosts[index]) presence.set(hostId, JSON.parse(hosts[index]));
      });
    }
    const multi = redisClient.multi();
    ids.forEach((id) => multi.get(redisKey(`market:game:${id}`)));
    const metadata = await multi.exec();
    const games = ids.flatMap((id, index) => {
      if (!metadata[index]) return [];
      const availableHosts = memberships[index].filter((hostId) => {
        const host = presence.get(hostId);
        return host?.state === 'idle' && controlGateway.connected(hostId);
      }).length;
      const game = JSON.parse(metadata[index]);
      return [{ id: game.id, source: game.source, title: game.title, availableHosts }];
    });
    res.json({ games: games.sort((a, b) => a.title.localeCompare(b.title)) });
  } catch (error) {
    log('error', 'Game catalog lookup failed', { error: safeError(error) });
    res.status(503).json({ error: 'Catalog unavailable' });
  }
});

app.get('/api/v1/ping', (_req, res) => {
  const configured = process.env.DEPLOYMENT_REGION || process.env.RAILWAY_REGION || 'local';
  const region = /^[A-Za-z0-9_-]{1,32}$/.test(configured) ? configured : 'local';
  res.json({ region, serverTime: Date.now() });
});

app.post('/api/v1/sessions', async (req, res) => {
  const parsed = SessionRequestSchema.safeParse(req.body);
  if (!parsed.success) {
    return res
      .status(400)
      .json({ error: 'Validation failed', issues: formatZodIssues(parsed.error) });
  }
  const playerId = await authenticatedPlayerId(req, config);
  if (!playerId) return res.status(401).json({ error: 'Unauthorized' });
  const request = parsed.data;
  const session = newSession(playerId, request);
  try {
    const locked = await redisClient.set(playerSessionKey(playerId), session.id, {
      NX: true,
      EX: sessionTtl(request.durationSeconds),
    });
    if (!locked) {
      marketMetrics.sessionOutcome('duplicate');
      return res.status(409).json({
        error: 'Player already has a session',
        sessionId: await redisClient.get(playerSessionKey(playerId)),
      });
    }
    const allocated = await allocateSession(session);
    if (allocated) {
      marketMetrics.sessionOutcome('allocated');
      return res.status(202).json({ session: allocated });
    }
    await releasePlayer(session);
    marketMetrics.sessionOutcome('unavailable');
    res.status(409).json({ error: 'All matching hosts became unavailable' });
  } catch (error) {
    await releasePlayer(session).catch(() => undefined);
    log('error', 'Session allocation failed', { error: safeError(error) });
    res.status(503).json({ error: 'Session service unavailable' });
  }
});

app.get('/api/v1/sessions/:sessionId', async (req, res) => {
  if (!/^[0-9a-f-]{36}$/i.test(req.params.sessionId))
    return res.status(400).json({ error: 'Invalid session ID' });
  const session = await loadSession(req.params.sessionId);
  const playerId = await authenticatedPlayerId(req, config);
  if (session && session.playerId !== playerId) return res.status(403).json({ error: 'Forbidden' });
  res.status(session ? 200 : 404).json(session ? { session } : { error: 'Session not found' });
});

app.get('/api/v1/sessions/:sessionId/bootstrap', async (req, res) => {
  const session = await loadSession(req.params.sessionId);
  if (!session) return res.status(404).json({ error: 'Session not found' });
  if (session.playerId !== (await authenticatedPlayerId(req, config)))
    return res.status(403).json({ error: 'Forbidden' });
  if (!['ready', 'active'].includes(session.state)) {
    return res.status(409).json({ error: 'Session is not ready', state: session.state });
  }
  const expiresAt = Date.now() + config.pairingTokenTtlSeconds * 1000;
  const pairingToken = signPairingToken(
    { roomId: session.roomId, sessionId: session.id, hostId: session.hostId, expiresAt },
    config.pairingTokenSecret,
  );
  res.json({
    roomId: session.roomId,
    sessionId: session.id,
    pairingToken,
    expiresAt,
    endsAt: session.endsAt,
    iceServers: await getIceServers(),
  });
});

app.delete('/api/v1/sessions/:sessionId', async (req, res) => {
  const session = await loadSession(req.params.sessionId);
  if (!session) return res.status(404).json({ error: 'Session not found' });
  if (session.playerId !== (await authenticatedPlayerId(req, config)))
    return res.status(403).json({ error: 'Forbidden' });
  if (['ended', 'failed'].includes(session.state)) return res.json({ session });
  session.state = 'ending';
  controlGateway.send(session.hostId, 'session.stop', session.id, { reason: 'player_requested' });
  await releasePlayer(session);
  marketMetrics.sessionOutcome('cancelled');
  await saveSession(session);
  res.status(202).json({ session });
});

app.post('/api/match/find', async (req, res) => {
  const parsed = MatchFindSchema.safeParse(req.body || {});
  if (!parsed.success) {
    return res.status(400).json({
      success: false,
      error: 'Validation failed',
      issues: formatZodIssues(parsed.error),
    });
  }
  const pairingCode = parsed.data.pairingCode.toLowerCase();
  const pairingHash = sha256(pairingCode);

  try {
    await pruneStaleIdleHosts();
    const requestedHostId = await redisClient.get(redisKey(`pairing:${pairingHash}`));
    if (!requestedHostId) {
      return res.status(404).json({ found: false, message: 'No hosts available' });
    }
    const claimedJson = await redisClient.eval(CLAIM_HOST_SCRIPT, {
      keys: [
        redisKey(`host:${requestedHostId}`),
        redisKey('idle_hosts'),
        redisKey(`host_reservations:${requestedHostId}`),
        redisKey(`pairing:${pairingHash}`),
        redisKey(`pairing-consumed:${requestedHostId}`),
      ],
      arguments: [
        requestedHostId,
        String(config.hostTtlSeconds),
        String(config.allocationReservationSeconds),
        pairingHash,
      ],
    });
    if (claimedJson) {
      const host = JSON.parse(claimedJson);
      const iceServers = await getIceServers();
      const sessionId = randomUUID();
      const expiresAt = Date.now() + config.pairingTokenTtlSeconds * 1000;
      const pairingToken = signPairingToken(
        { roomId: host.roomId, sessionId, hostId: requestedHostId, expiresAt },
        config.pairingTokenSecret,
      );
      return res.json({
        found: true,
        roomId: host.roomId,
        sessionId,
        pairingToken,
        expiresAt,
        iceServers,
      });
    }
    return res.status(404).json({ found: false, message: 'Host is unavailable' });
  } catch (err) {
    log('error', 'Match Error', { requestId: req.id, error: safeError(err) });
    res.status(500).json({ error: 'Internal server error' });
  }
});

app.use((req, res) => res.status(404).json({ success: false, error: 'Not found' }));
app.use((error, req, res, _next) => {
  log('warn', 'Rejected malformed HTTP request', { requestId: req.id, error: error && error.type });
  res
    .status(error && error.status === 413 ? 413 : 400)
    .json({ success: false, error: 'Malformed request' });
});

function createRedis(urlString) {
  return createClient({
    url: urlString,
    socket: {
      reconnectStrategy: (retries) => {
        const delay = Math.min(1000 + retries * 50, 5000);
        return delay;
      },
    },
  });
}

const redisClient = createRedis(config.redisUrl);
const apiRateLimiter = RateLimiter(redisClient, config.redisPrefix, config.env === 'production');
redisClient.on('error', (err) => log('error', 'Redis client error', { error: safeError(err) }));
const controlGateway = new HostControlGateway({
  authenticate: (request, hostId) =>
    hostCredentialValid(config, hostId, bearerToken(request.headers)),
  onEvent: handleHostEvent,
  onConnection: (hostId, connected) => {
    log('info', `Host control ${connected ? 'connected' : 'disconnected'}`, { hostId });
    marketMetrics.setControlHosts(controlGateway.connectedCount());
    handleControlConnection(hostId, connected).catch((error) =>
      log('warn', 'Host control grace update failed', { hostId, error: safeError(error) }),
    );
  },
  log,
});

async function expireSessions() {
  const now = Date.now();
  const ids = await redisClient.zRangeByScore(redisKey('market:session-deadlines'), 0, now);
  for (const id of ids) {
    const session = await loadSession(id);
    if (session?.state === 'active') {
      session.state = 'ending';
      controlGateway.send(session.hostId, 'session.stop', id, { reason: 'duration_expired' });
      await saveSession(session);
    }
    await redisClient.zRem(redisKey('market:session-deadlines'), id);
  }
}

async function expirePreparations() {
  const key = redisKey('market:session-prepare-deadlines');
  const ids = await redisClient.zRangeByScore(key, 0, Date.now());
  for (const id of ids) {
    const session = await loadSession(id);
    if (session && ['reserved', 'preparing'].includes(session.state)) {
      controlGateway.send(session.hostId, 'session.stop', id, { reason: 'prepare_timeout' });
      await failOrRetrySession(session, 'prepare_timeout');
    } else await redisClient.zRem(key, id);
  }
}

async function expireDisconnectedHosts() {
  const key = redisKey('market:host-disconnect-deadlines');
  const hostIds = await redisClient.zRangeByScore(key, 0, Date.now());
  for (const hostId of hostIds) {
    if (!controlGateway.connected(hostId)) {
      const sessionId = await redisClient.get(hostLeaseKey(hostId));
      const session = sessionId ? await loadSession(sessionId) : null;
      if (session && !['ended', 'failed'].includes(session.state))
        await failOrRetrySession(session, 'host_disconnected');
    }
    await redisClient.zRem(key, hostId);
  }
}

async function startServer() {
  // Bind to PORT first so Railway's health check passes immediately.
  // Redis connection happens after — a slow Redis startup no longer kills the container.
  const port = process.env.PORT || config.mmPort;
  let listener;
  await new Promise((resolve, reject) => {
    listener = app.listen(port, config.bindHost, () => {
      console.log(`Matchmaker server is running on ${config.bindHost}:${port}`);
      resolve();
    });
    listener.headersTimeout = 10000;
    listener.requestTimeout = 15000;
    listener.keepAliveTimeout = 5000;
    listener.maxHeadersCount = 64;
    listener.once('error', reject);
  });
  controlGateway.attach(listener);

  try {
    await redisClient.connect();
    console.log('Connected to Redis');
  } catch (error) {
    log('error', 'Failed to connect to Redis; retrying in background', { error: safeError(error) });
    // Don't exit — the redis client's reconnectStrategy will keep retrying
  }

  try {
    await pruneStaleIdleHosts();
  } catch (_) {}
  setInterval(
    () =>
      pruneStaleIdleHosts().catch((e) =>
        log('error', 'Periodic stale-host pruning failed', { error: safeError(e) }),
      ),
    10000,
  );
  setInterval(
    () => expireSessions().catch((error) => log('error', 'Session expiry failed', { error })),
    1000,
  ).unref();
  setInterval(
    () =>
      expirePreparations().catch((error) =>
        log('error', 'Session preparation expiry failed', { error }),
      ),
    1000,
  ).unref();
  setInterval(
    () =>
      expireDisconnectedHosts().catch((error) =>
        log('error', 'Disconnected host expiry failed', { error: safeError(error) }),
      ),
    1000,
  ).unref();
}

startServer().catch((err) => fatalExit('startServer', err));
