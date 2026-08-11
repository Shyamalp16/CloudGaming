const http = require('http');
const WebSocket = require('ws');
const crypto = require('crypto');
const { createClient } = require('redis');
const { config } = require('./config');
const { logger } = require('./logger');
const { validateRedisEnvelope, validateSignalingMessage } = require('./validation');
const { metricsHandler } = require('./metrics');
const { RateLimiter } = require('./rateLimiter');
const {
  setActiveConnections,
  setLocalRooms,
  setRedisUp,
  setCircuitBreakerOpen,
  incMessagesForwarded,
  incSchemaRejects,
  incRateLimitDrops,
  incBackpressureCloses,
  startRedisTimer,
  startFanoutTimer,
} = require('./metrics');
const jwt = require('jsonwebtoken');
const { createRemoteJWKSet, jwtVerify } = require('jose');
const { atomicJoin, atomicLeave } = require('./redisScripts');
const { signPairingToken, verifyPairingToken } = require('./sessionTokens');
const {
  bearerToken,
  extractProtocolCredential,
  hostCredentialValid,
  originAllowed,
  requestIsSecure,
  secureEqual,
} = require('./security');

function log(level, message, context) {
  const validLevels = ['fatal', 'error', 'warn', 'info', 'debug', 'trace'];
  const method = validLevels.includes(level) ? level : 'info';
  const ctx = context || {};
  if (ctx.error && !ctx.err) {
    ctx.err = ctx.error;
  }
  if (typeof logger[method] === 'function') {
    logger[method](ctx, message);
  } else {
    logger.info(ctx, message);
  }
}

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
const subscriber = redisClient.duplicate();
const serverInstanceId = crypto.randomUUID();
const rateLimiter = RateLimiter(redisClient, config.redisPrefix, config.env === 'production');
const redisKey = (name) => `${config.redisPrefix}${name}`;
let redisCircuitOpenUntil = 0;
let cachedJwks = null;
let localConnectionCount = 0;
let heartbeatTimer = null;

function getJwks() {
  if (!cachedJwks) cachedJwks = createRemoteJWKSet(new URL(config.jwt.jwksUrl));
  return cachedJwks;
}

redisClient.on('error', (err) => {
  setRedisUp(false);
  log('error', 'Redis client error', { err });
});
subscriber.on('error', (err) => {
  setRedisUp(false);
  log('error', 'Redis subscriber error', { err });
});

let redisFailureCount = 0;
function noteRedisFailure() {
  redisFailureCount += 1;
  if (redisFailureCount >= config.cbErrorThreshold) {
    redisCircuitOpenUntil = Date.now() + config.cbOpenMs;
    setCircuitBreakerOpen(true);
    log('warn', 'Redis circuit opened', { until: redisCircuitOpenUntil });
  }
}
function noteRedisSuccess() {
  redisFailureCount = 0;
  if (redisCircuitOpenUntil && Date.now() >= redisCircuitOpenUntil) {
    redisCircuitOpenUntil = 0;
    setCircuitBreakerOpen(false);
  }
  setRedisUp(true);
}

// =============================
// Combined HTTP + WebSocket Server
// =============================
// Railway routes all traffic (including health checks) to a single PORT.
// We attach both the WebSocket server and the health/metrics endpoints to one
// HTTP server so there is no port conflict with the old separate health server.
let draining = false;

const httpServer = http.createServer(async (req, res) => {
  res.setHeader('Cache-Control', 'no-store');
  res.setHeader('X-Content-Type-Options', 'nosniff');
  res.setHeader('Referrer-Policy', 'no-referrer');
  if (req.url === '/healthz') {
    res.writeHead(200, { 'Content-Type': 'text/plain' });
    res.end('ok');
    return;
  }
  if (req.url === '/readyz') {
    try {
      const pong = await redisClient.ping();
      if (pong === 'PONG' && !draining) {
        res.writeHead(200, { 'Content-Type': 'text/plain' });
        res.end('ready');
      } else {
        res.writeHead(503, { 'Content-Type': 'text/plain' });
        res.end('not-ready');
      }
    } catch (_) {
      res.writeHead(503, { 'Content-Type': 'text/plain' });
      res.end('not-ready');
    }
    return;
  }
  if (req.url === '/metrics') {
    if (!config.metricsSecret || !secureEqual(bearerToken(req.headers), config.metricsSecret)) {
      res.writeHead(404, { 'Content-Type': 'text/plain' });
      res.end('not found');
      return;
    }
    return metricsHandler(req, res);
  }
  // Non-WebSocket HTTP requests get a standard 426
  res.writeHead(426, { 'Content-Type': 'text/plain' });
  res.end('Upgrade Required');
});

const server = new WebSocket.Server({
  server: httpServer,
  maxPayload: config.messageMaxBytes,
  perMessageDeflate: false,
  handleProtocols(protocols) {
    return protocols.has(config.subprotocol) ? config.subprotocol : false;
  },
});

const localRooms = new Map();

const ROOM_ID_REGEX = /^[a-f0-9]{32}$/i;
function validateRoomId(roomId) {
  if (typeof roomId !== 'string') return false;
  if (roomId.length === 0 || roomId.length > config.roomIdMaxLength) return false;
  return ROOM_ID_REGEX.test(roomId);
}

function safeClientId() {
  if (crypto.randomUUID) return `client:${crypto.randomUUID()}`;
  return `client:${Date.now()}:${crypto.randomBytes(8).toString('hex')}`;
}

function shouldRoute(senderRole, senderSessionId, target) {
  if (!config.enableSessionAuth) return true;
  if (senderRole === target.role) return false;
  if (!senderSessionId) return false;
  if (senderRole === 'host')
    return target.role === 'player' && target.sessionId === senderSessionId;
  return senderRole === 'player' && target.role === 'host';
}

function messageAllowedForRole(role, message) {
  if (role === 'player') {
    if (['offer', 'candidate', 'stream-profile'].includes(message.type)) return true;
    return message.type === 'control' && ['terminate', 'ping'].includes(message.action);
  }
  if (role === 'host') {
    if (['answer', 'candidate', 'stream-profile'].includes(message.type)) return true;
    return (
      message.type === 'control' &&
      [
        'terminate',
        'replace',
        'profile-accepted',
        'profile-rejected',
        'schema-error',
        'ping',
      ].includes(message.action)
    );
  }
  return false;
}

async function handleRedisMessage(message, channel) {
  try {
    const channelPrefix = redisKey('room:');
    if (typeof channel !== 'string' || !channel.startsWith(channelPrefix)) return;
    const roomId = channel.slice(channelPrefix.length);
    if (!validateRoomId(roomId)) return;
    let payload;
    try {
      payload = JSON.parse(message);
    } catch (e) {
      log('warn', 'Dropping non-JSON message from Redis', { channel, err: e });
      return;
    }
    const validation = validateRedisEnvelope(payload);
    if (!validation.ok) {
      log('warn', 'Dropping invalid Redis signaling envelope', { channel });
      return;
    }
    const { senderId, senderRole, sessionId, data, originServerId, hostId } = validation.data;
    if (originServerId && originServerId === serverInstanceId) {
      return;
    }
    const expectedOwner = senderRole === 'host' ? hostId : sessionId;
    const [leaseOwner, roleOwner] = await Promise.all([
      redisClient.get(redisKey(`room-client:${senderId}`)),
      redisClient.get(redisKey(`room-role:${roomId}:${senderRole}`)),
    ]);
    if (leaseOwner !== expectedOwner || roleOwner !== `${expectedOwner}|${senderId}`) {
      log('warn', 'Dropping Redis envelope without an active authenticated sender lease');
      return;
    }
    const clientsInRoom = localRooms.get(roomId);
    if (!clientsInRoom || clientsInRoom.size === 0) return;
    const dataStr = JSON.stringify(data);
    const endFanout = startFanoutTimer();
    clientsInRoom.forEach((client) => {
      if (
        client.clientId !== senderId &&
        client.readyState === WebSocket.OPEN &&
        client.hostId === hostId &&
        shouldRoute(senderRole, sessionId, client)
      ) {
        if (client.bufferedAmount > config.backpressureCloseThresholdBytes) {
          log('warn', 'Closing client due to excessive backpressure', {
            clientId: client.clientId,
            roomId,
          });
          incBackpressureCloses();
          try {
            client.close(1013, 'Server overloaded');
          } catch (_) {}
          return;
        }
        try {
          client.send(dataStr);
        } catch (e) {
          log('warn', 'Failed to forward message to client', {
            clientId: client.clientId,
            roomId,
            err: e,
          });
        }
      }
    });
    endFanout();
    incMessagesForwarded();
  } catch (error) {
    log('error', 'Error handling Redis message', { channel, err: error });
  }
}

async function handleNewConnection(ws, request) {
  let closedDuringSetup = false;
  const setupCloseHandler = () => {
    closedDuringSetup = true;
  };
  ws.once('close', setupCloseHandler);
  try {
    if (Date.now() < redisCircuitOpenUntil) {
      log('warn', 'Refusing connection due to Redis circuit open');
      ws.close(1013, 'Service unavailable');
      return;
    }
    if (!request || !request.url || !request.headers || !request.headers.host) {
      log('warn', 'Malformed connection request');
      ws.close(1008, 'Malformed request');
      return;
    }
    const ip = (request.socket && request.socket.remoteAddress) || 'unknown';
    const allowedConn = await rateLimiter.allow({
      namespace: 'conn',
      id: ip,
      limit: config.rateLimitConnPer10s,
      periodSeconds: 10,
    });
    if (!allowedConn) {
      log('warn', 'IP connection rate-limited', { ip });
      ws.close(1013, 'Rate limited');
      return;
    }
    const protocols = request.headers['sec-websocket-protocol'];
    const offeredProtocols =
      typeof protocols === 'string' ? protocols.split(',').map((value) => value.trim()) : [];
    if (!offeredProtocols.includes(config.subprotocol)) {
      ws.close(1008, 'Subprotocol required');
      return;
    }
    const protocolValues = (prefix) =>
      offeredProtocols
        .filter((value) => value.startsWith(prefix))
        .map((value) => value.slice(prefix.length));
    const roomValues = protocolValues('cg-room.');
    const roleValues = protocolValues('cg-role.');
    const hostValues = protocolValues('cg-host.');
    if (roomValues.length !== 1 || roleValues.length !== 1 || hostValues.length > 1) {
      ws.close(1008, 'Invalid connection metadata');
      return;
    }
    const roomIdValue = roomValues[0];
    const roomId = typeof roomIdValue === 'string' ? roomIdValue.toLowerCase() : roomIdValue;
    const role = roleValues[0];
    const hostIdValue = hostValues[0];
    const hostIdParameter =
      typeof hostIdValue === 'string' ? hostIdValue.toLowerCase() : hostIdValue;
    if (!validateRoomId(roomId)) {
      log('warn', 'Invalid or missing roomId on connection', { roomId });
      ws.close(1008, 'Invalid roomId');
      return;
    }
    if (!['host', 'player'].includes(role)) {
      ws.close(1008, 'Invalid role');
      return;
    }
    if (config.requireWss && !requestIsSecure(request, config.trustedProxyIps)) {
      log('warn', 'Rejected connection that did not arrive over trusted TLS');
      ws.close(1008, 'WSS required');
      return;
    }
    const origin = request.headers.origin;
    if (role === 'player' && !originAllowed(origin, config.allowedOrigins)) {
      log('warn', 'Origin not allowed', { origin });
      ws.close(1008, 'Origin not allowed');
      return;
    }
    if (role === 'host' && origin && !originAllowed(origin, config.allowedOrigins)) {
      ws.close(1008, 'Origin not allowed');
      return;
    }
    if (
      (role === 'host' && hostValues.length !== 1) ||
      (role === 'player' && hostValues.length !== 0)
    ) {
      ws.close(1008, 'Invalid role metadata');
      return;
    }

    let authenticatedSessionId;
    let pairingClaims;
    let authenticatedHostId;
    if (config.enableSessionAuth) {
      if (role === 'host') {
        const credential = bearerToken(request.headers);
        if (!hostCredentialValid(config, hostIdParameter, credential)) {
          ws.close(1008, 'Unauthorized host');
          return;
        }
        const registeredRoom = await redisClient.get(redisKey(`host-room:${hostIdParameter}`));
        let marketplaceRoom = false;
        if (registeredRoom !== roomId) {
          const lease = await redisClient.get(redisKey(`market:host-lease:${hostIdParameter}`));
          const rawSession = lease
            ? await redisClient.get(redisKey(`market:session:${lease}`))
            : null;
          if (rawSession) {
            const session = JSON.parse(rawSession);
            marketplaceRoom =
              session.hostId === hostIdParameter &&
              session.roomId === roomId &&
              !['ended', 'failed'].includes(session.state);
          }
        }
        if (registeredRoom !== roomId && !marketplaceRoom) {
          ws.close(1008, 'Host is not registered for this room');
          return;
        }
        authenticatedHostId = hostIdParameter;
      } else {
        const pairingCredential = extractProtocolCredential(protocols, 'cg-pairing.');
        pairingClaims = verifyPairingToken(pairingCredential, config.pairingTokenSecret);
        if (!pairingClaims || pairingClaims.roomId !== roomId) {
          ws.close(1008, 'Invalid or expired pairing token');
          return;
        }
        authenticatedSessionId = pairingClaims.sessionId;
        authenticatedHostId = pairingClaims.hostId;
      }
    }
    if (!config.enableSessionAuth) {
      // Development-only mode still uses well-formed internal identities so
      // Redis fanout cannot bypass envelope validation.
      authenticatedHostId = `${roomId.slice(0, 8)}-${roomId.slice(8, 12)}-${roomId.slice(12, 16)}-${roomId.slice(16, 20)}-${roomId.slice(20)}`;
      if (role === 'player') authenticatedSessionId = crypto.randomUUID();
    }
    if (config.enableAuth && role === 'player') {
      let token = extractProtocolCredential(protocols, 'cg-access.');
      if (!token) token = bearerToken(request.headers);
      if (!token) {
        log('warn', 'Missing JWT');
        ws.close(1008, 'Unauthorized');
        return;
      }
      try {
        let payload;
        if (config.jwt.jwksUrl) {
          const { payload: pl } = await jwtVerify(token, getJwks(), {
            issuer: config.jwt.issuer,
            audience: config.jwt.audience,
            algorithms: [config.jwt.alg],
          });
          payload = pl;
        } else {
          payload = jwt.verify(token, config.jwt.secret, {
            algorithms: [config.jwt.alg],
            issuer: config.jwt.issuer,
            audience: config.jwt.audience,
          });
        }
        const allowedRooms = payload[config.jwt.roomsClaim];
        if (
          typeof payload.sub !== 'string' ||
          payload.sub.length === 0 ||
          payload.sub.length > 128 ||
          payload.role !== role ||
          !Array.isArray(allowedRooms) ||
          allowedRooms.length > 32 ||
          !allowedRooms.every((value) => typeof value === 'string' && validateRoomId(value)) ||
          !allowedRooms.includes(roomId)
        ) {
          log('warn', 'JWT does not authorize room', { roomId });
          ws.close(1008, 'Forbidden');
          return;
        }
        ws.user = { sub: payload.sub };
      } catch (e) {
        log('warn', 'JWT verification failed', { err: e });
        ws.close(1008, 'Unauthorized');
        return;
      }
    }

    if (pairingClaims) {
      try {
        const ttlMs = Math.max(1, pairingClaims.exp - Date.now());
        const claimed = await redisClient.set(redisKey(`pairing-used:${pairingClaims.jti}`), '1', {
          NX: true,
          PX: ttlMs,
        });
        if (claimed !== 'OK') {
          ws.close(1008, 'Pairing token already used');
          return;
        }
      } catch (e) {
        log('error', 'Failed to claim pairing credential', { roomId, err: e });
        ws.close(1011, 'Pairing service unavailable');
        return;
      }
    }

    const roomKey = redisKey(`room:${roomId}`);
    const clientId = safeClientId();
    const roleKey = redisKey(`room-role:${roomId}:${role}`);
    const leaseKey = redisKey(`room-client:${clientId}`);
    const roleOwner = role === 'host' ? authenticatedHostId : authenticatedSessionId;

    try {
      const end = startRedisTimer();
      const result = await atomicJoin(
        redisClient,
        roomKey,
        roleKey,
        leaseKey,
        clientId,
        config.roomCapacity,
        config.roomTtlSeconds,
        roleOwner,
        redisKey('room-client:'),
      );
      end();
      noteRedisSuccess();
      if (result === -1) {
        log('info', 'Room is full, rejecting connection', { roomId, clientId });
        ws.close(1000, 'Room is full');
        return;
      }
      if (result === -2) {
        log('info', 'Room role is already occupied', { roomId, role });
        ws.close(1008, 'Role already connected');
        return;
      }
    } catch (e) {
      log('error', 'Redis error during join', { roomId, clientId, err: e });
      noteRedisFailure();
      ws.close(1011, 'Internal error');
      return;
    }
    if (closedDuringSetup || ws.readyState !== WebSocket.OPEN) {
      try {
        await atomicLeave(
          redisClient,
          roomKey,
          roleKey,
          leaseKey,
          clientId,
          config.roomTtlSeconds,
          roleOwner,
        );
      } catch (_) {}
      return;
    }

    ws.roomId = roomId;
    ws.clientId = clientId;
    ws.role = role;
    ws.sessionId = authenticatedSessionId;
    ws.hostId = authenticatedHostId;
    ws.roleKey = roleKey;
    ws.leaseKey = leaseKey;
    ws.roleOwner = roleOwner;
    ws.isAlive = true;
    ws._rate = { tokens: config.rateLimitMessagesPer10s, lastRefill: Date.now() };

    const existingLocal = localRooms.get(roomId);
    if (existingLocal) {
      for (const existing of existingLocal) {
        if (existing.role === role && existing.roleOwner === roleOwner) {
          try {
            existing.close(1000, 'Replaced by authenticated reconnect');
          } catch (_) {}
        }
      }
    }
    if (!localRooms.has(roomId)) localRooms.set(roomId, new Set());
    localRooms.get(roomId).add(ws);
    localConnectionCount++;
    setActiveConnections(localConnectionCount);
    setLocalRooms(localRooms.size);

    log('info', 'Client joined room', {
      clientId,
      roomId,
      localCount: localRooms.get(roomId).size,
    });
    if (pairingClaims && ws.readyState === WebSocket.OPEN) {
      const nextToken = signPairingToken(
        {
          roomId,
          sessionId: authenticatedSessionId,
          hostId: authenticatedHostId,
          expiresAt: Date.now() + config.pairingTokenTtlSeconds * 1000,
        },
        config.pairingTokenSecret,
      );
      ws.send(
        JSON.stringify({
          type: 'control',
          sessionId: authenticatedSessionId,
          action: 'session-ready',
          payload: { pairingToken: nextToken },
        }),
      );
    }

    ws.on('pong', () => {
      ws.isAlive = true;
      log('debug', 'Received pong from client', { clientId: ws.clientId });
    });

    ws._messageChain = Promise.resolve();
    ws._pendingMessages = 0;
    ws.on('message', (message) => {
      if (ws._pendingMessages >= 64) {
        incBackpressureCloses();
        try {
          ws.close(1013, 'Too many pending messages');
        } catch (_) {}
        return;
      }
      ws._pendingMessages++;
      ws._messageChain = ws._messageChain
        .then(() => handleMessage(ws, roomKey, message))
        .catch((err) => log('error', 'Queued message handler failed', { clientId, roomId, err }))
        .finally(() => {
          ws._pendingMessages--;
        });
    });
    ws.off('close', setupCloseHandler);
    ws.on('close', () => handleDisconnection(ws, roomKey));
    ws.on('error', (err) => {
      log('warn', 'WebSocket error', { clientId: ws.clientId, roomId: ws.roomId, err });
    });
  } catch (error) {
    log('error', 'Unhandled error during connection setup', { err: error });
    try {
      ws.close(1011, 'Internal server error');
    } catch (_) {}
  }
}

function refillTokens(rate) {
  const now = Date.now();
  const elapsed = now - rate.lastRefill;
  if (elapsed <= 0) return;
  const tokensToAdd = (config.rateLimitMessagesPer10s / 10000) * elapsed;
  rate.tokens = Math.min(config.rateLimitMessagesPer10s, rate.tokens + tokensToAdd);
  rate.lastRefill = now;
}

async function handleMessage(ws, roomKey, message) {
  try {
    if (ws._disconnected) return;
    if (typeof message === 'string') {
      if (Buffer.byteLength(message) > config.messageMaxBytes) {
        log('warn', 'Dropping oversized text message', {
          clientId: ws.clientId,
          roomId: ws.roomId,
        });
        return;
      }
    } else if (Buffer.isBuffer(message)) {
      if (message.length > config.messageMaxBytes) {
        log('warn', 'Dropping oversized binary message', {
          clientId: ws.clientId,
          roomId: ws.roomId,
        });
        return;
      }
      message = message.toString('utf8');
    } else {
      return;
    }

    // Rate limiting
    refillTokens(ws._rate);
    if (ws._rate.tokens <= 0) {
      log('warn', 'Rate limit exceeded, dropping message', {
        clientId: ws.clientId,
        roomId: ws.roomId,
      });
      return;
    }
    ws._rate.tokens -= 1;

    // Backpressure check
    if (ws.bufferedAmount > config.backpressureCloseThresholdBytes) {
      log('warn', 'Closing client due to excessive backpressure (sender)', {
        clientId: ws.clientId,
        roomId: ws.roomId,
      });
      try {
        ws.close(1013, 'Server overloaded');
      } catch (_) {}
      return;
    }

    let parsedMessage;
    try {
      parsedMessage = JSON.parse(message);
    } catch (e) {
      log('warn', 'Dropping non-JSON client message', { clientId: ws.clientId, roomId: ws.roomId });
      return;
    }

    // Schema validation
    const validation = validateSignalingMessage(parsedMessage);
    if (!validation.ok) {
      log('warn', 'Dropping invalid signaling message', {
        clientId: ws.clientId,
        roomId: ws.roomId,
      });
      // Optionally send a control error
      try {
        ws.send(JSON.stringify({ type: 'control', action: 'schema-error' }));
      } catch (_) {}
      incSchemaRejects();
      return;
    }
    const messageSessionId = validation.data.sessionId;
    if (!messageAllowedForRole(ws.role, validation.data)) {
      log('warn', 'Dropping signaling action not authorized for role', { clientId: ws.clientId });
      return;
    }
    if (config.enableSessionAuth) {
      if (!messageSessionId) {
        log('warn', 'Dropping signaling message without sessionId', {
          clientId: ws.clientId,
          roomId: ws.roomId,
        });
        return;
      }
      if (ws.role === 'player' && messageSessionId !== ws.sessionId) {
        log('warn', 'Dropping stale or replayed player message', {
          clientId: ws.clientId,
          roomId: ws.roomId,
        });
        return;
      }
      if (ws.role === 'host') {
        const peers = localRooms.get(ws.roomId);
        let known =
          peers &&
          [...peers].some((peer) => peer.role === 'player' && peer.sessionId === messageSessionId);
        if (!known) {
          const owner = await redisClient.get(redisKey(`room-role:${ws.roomId}:player`));
          known = typeof owner === 'string' && owner.startsWith(`${messageSessionId}|`);
        }
        if (!known) return;
        ws.currentSessionId = messageSessionId;
      }
    }

    // IP and room message rate limits
    const ip = (ws._socket && ws._socket.remoteAddress) || 'unknown';
    let allowedMsg = true;
    try {
      allowedMsg = await rateLimiter.allow({
        namespace: 'msg-ip',
        id: ip,
        limit: config.rateLimitIpMsgsPer10s,
        periodSeconds: 10,
      });
      if (allowedMsg) {
        allowedMsg = await rateLimiter.allow({
          namespace: 'msg-room',
          id: ws.roomId,
          limit: config.rateLimitRoomMsgsPer10s,
          periodSeconds: 10,
        });
      }
    } catch (e) {
      log('warn', 'Rate limiter error on message', { ip, roomId: ws.roomId, err: e });
    }
    if (!allowedMsg) {
      log('warn', 'Message rate-limited', { clientId: ws.clientId, roomId: ws.roomId, ip });
      incRateLimitDrops();
      return;
    }

    // Local fanout for same-instance peers to reduce dependency on pub/sub timing
    try {
      const peers = localRooms.get(ws.roomId);
      if (peers && peers.size > 0) {
        const payload = JSON.stringify(validation.data);
        peers.forEach((peer) => {
          if (
            peer !== ws &&
            peer.readyState === WebSocket.OPEN &&
            shouldRoute(ws.role, messageSessionId, peer)
          ) {
            if (peer.bufferedAmount <= config.backpressureCloseThresholdBytes) {
              try {
                peer.send(payload);
              } catch (_) {}
            }
          }
        });
      }
    } catch (_) {}

    try {
      const ePub = startRedisTimer();
      await redisClient.publish(
        roomKey,
        JSON.stringify({
          senderId: ws.clientId,
          senderRole: ws.role,
          sessionId: messageSessionId,
          hostId: ws.hostId,
          data: validation.data,
          originServerId: serverInstanceId,
        }),
      );
      ePub();
      noteRedisSuccess();
    } catch (e) {
      log('error', 'Failed to publish to Redis', {
        roomId: ws.roomId,
        clientId: ws.clientId,
        err: e,
      });
      noteRedisFailure();
    }
  } catch (error) {
    log('error', 'Unhandled error in message handler', {
      clientId: ws.clientId,
      roomId: ws.roomId,
      err: error,
    });
  }
}

async function handleDisconnection(ws, roomKey) {
  if (ws._disconnected) return;
  ws._disconnected = true;
  const roomId = ws.roomId;
  const clientId = ws.clientId;
  log('info', 'Client disconnected', { clientId, roomId });

  // Remove from local map
  try {
    const roomClients = localRooms.get(roomId);
    if (roomClients) {
      if (roomClients.delete(ws)) localConnectionCount = Math.max(0, localConnectionCount - 1);
      if (roomClients.size === 0) {
        localRooms.delete(roomId);
        log('info', 'Room now empty on this instance', { roomId });
      }
    }
  } catch (e) {
    log('warn', 'Local room cleanup failed', { roomId, err: e });
  }
  setActiveConnections(localConnectionCount);
  setLocalRooms(localRooms.size);
  if (!roomId || !clientId) return;

  // Redis cleanup and notify peers
  try {
    const end = startRedisTimer();
    await atomicLeave(
      redisClient,
      roomKey,
      ws.roleKey,
      ws.leaseKey,
      clientId,
      config.roomTtlSeconds,
      ws.roleOwner,
    );
    end();
    noteRedisSuccess();
    const disconnectSessionId = ws.sessionId || ws.currentSessionId;
    if (disconnectSessionId) {
      await redisClient.publish(
        roomKey,
        JSON.stringify({
          senderId: clientId,
          senderRole: ws.role,
          sessionId: disconnectSessionId,
          hostId: ws.hostId,
          data: { type: 'peer-disconnected', sessionId: disconnectSessionId },
          originServerId: serverInstanceId,
        }),
      );
    }
  } catch (e) {
    log('warn', 'Redis cleanup or notify failed', { roomId, clientId, err: e });
    noteRedisFailure();
  }
}

// =============================
// Bootstrap
// =============================
async function main() {
  try {
    await redisClient.connect();
    await subscriber.connect();
    log('info', 'Connected to Redis');
    setRedisUp(true);
  } catch (e) {
    log('error', 'Failed to connect to Redis', { err: e });
    process.exit(1);
  }

  try {
    await subscriber.pSubscribe(redisKey('room:*'), handleRedisMessage);
    log('info', 'Subscribed to Redis channel pattern');
  } catch (e) {
    log('error', 'Failed to subscribe to Redis pattern', { err: e });
    process.exit(1);
  }

  server.on('connection', (ws, request) => {
    if (draining) {
      try {
        ws.close(config.shutdownCloseCode, 'Server draining');
      } catch (_) {}
      return;
    }
    handleNewConnection(ws, request);
  });
  server.on('error', (err) => log('error', 'WebSocket server error', { err }));
  heartbeatTimer = setInterval(() => {
    server.clients.forEach((ws) => {
      if (ws.readyState !== WebSocket.OPEN) return;
      if (!ws.isAlive) {
        log('warn', 'Terminating unresponsive client', {
          clientId: ws.clientId,
          roomId: ws.roomId,
        });
        try {
          ws.terminate();
        } catch (_) {}
        return;
      }
      ws.isAlive = false;
      if (ws.roleKey && ws.clientId) {
        Promise.all([
          redisClient.expire(ws.roleKey, config.roomTtlSeconds),
          redisClient.expire(ws.leaseKey, config.roomTtlSeconds),
          redisClient.expire(redisKey(`room:${ws.roomId}`), config.roomTtlSeconds),
        ]).catch((err) => log('warn', 'Failed to refresh role ownership', { err }));
      }
      try {
        ws.ping();
      } catch (e) {
        log('warn', 'Failed to send ping', { clientId: ws.clientId, err: e });
      }
    });
  }, config.heartbeatIntervalMs);
  if (typeof heartbeatTimer.unref === 'function') heartbeatTimer.unref();

  // Start the combined HTTP+WS server on Railway's injected PORT (or local fallback)
  const listenPort = process.env.PORT || config.wsPort;
  httpServer.listen(listenPort, config.bindHost, () => {
    log('info', 'Scalable Signaling Server listening', {
      port: listenPort,
      bindHost: config.bindHost,
    });
  });
}

main().catch((err) => {
  log('error', 'Unhandled error in main()', { err });
  process.exit(1);
});

// Graceful shutdown
async function shutdown(exitCode = 0) {
  if (draining) return;
  const forcedExit = setTimeout(() => process.exit(exitCode || 1), config.drainTimeoutMs + 3000);
  log('info', 'Shutting down gracefully...');
  // Enter drain mode
  draining = true;
  if (heartbeatTimer) clearInterval(heartbeatTimer);
  try {
    server.close();
  } catch (_) {}

  // Best-effort close of all clients and Redis membership cleanup, and wait for close frames to flush
  const closePromises = [];
  localRooms.forEach((clients, roomId) => {
    clients.forEach((ws) => {
      try {
        ws.close(config.shutdownCloseCode, 'Server draining');
      } catch (_) {}
      const roomKey = redisKey(`room:${roomId}`);
      const waitForClose = new Promise((resolve) => {
        let resolved = false;
        const done = () => {
          if (!resolved) {
            resolved = true;
            resolve();
          }
        };
        try {
          ws.once('close', () => done());
        } catch (_) {
          done();
        }
        // Fallback in case close event doesn't arrive in time
        setTimeout(done, Math.min(1000, config.drainTimeoutMs));
      });
      closePromises.push(
        (async () => {
          try {
            let e1 = startRedisTimer();
            await atomicLeave(
              redisClient,
              roomKey,
              ws.roleKey,
              ws.leaseKey,
              ws.clientId,
              config.roomTtlSeconds,
              ws.roleOwner,
            );
            e1();
          } catch (_) {}
          await waitForClose;
        })(),
      );
    });
  });

  try {
    await Promise.race([
      Promise.all(closePromises).catch(() => {}),
      new Promise((resolve) => setTimeout(resolve, config.drainTimeoutMs)),
    ]);
  } catch (_) {}

  try {
    await Promise.race([
      subscriber.quit(),
      new Promise((_, reject) =>
        setTimeout(() => reject(new Error('subscriber quit timeout')), 2000),
      ),
    ]);
  } catch (_) {}
  try {
    await Promise.race([
      redisClient.quit(),
      new Promise((_, reject) => setTimeout(() => reject(new Error('Redis quit timeout')), 2000)),
    ]);
  } catch (_) {}
  clearTimeout(forcedExit);
  process.exit(exitCode);
}

process.on('SIGINT', () => shutdown(0));
process.on('SIGTERM', () => shutdown(0));
process.on('uncaughtException', (err) => {
  log('fatal', 'Uncaught exception; terminating', { err });
  shutdown(1).catch(() => process.exit(1));
});
process.on('unhandledRejection', (err) => {
  log('fatal', 'Unhandled rejection; terminating', { err });
  shutdown(1).catch(() => process.exit(1));
});
