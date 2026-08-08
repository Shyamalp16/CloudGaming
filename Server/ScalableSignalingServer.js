const http = require('http');
const WebSocket = require('ws');
const url = require('url');
const crypto = require('crypto');
const { createClient } = require('redis');
const { config } = require('./config');
const { logger } = require('./logger');
const { validateSignalingMessage } = require('./validation');
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
			}
		}
	});
}

const redisClient = createRedis(config.redisUrl);
const subscriber = redisClient.duplicate();
const serverInstanceId = (crypto.randomUUID && crypto.randomUUID()) || `srv:${Date.now()}:${Math.random().toString(36).slice(2, 10)}`;
const rateLimiter = RateLimiter(redisClient);
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
		return metricsHandler(req, res);
	}
	// Non-WebSocket HTTP requests get a standard 426
	res.writeHead(426, { 'Content-Type': 'text/plain' });
	res.end('Upgrade Required');
});

const server = new WebSocket.Server({ server: httpServer, maxPayload: config.messageMaxBytes });

const localRooms = new Map();

const ROOM_ID_REGEX = /^[A-Za-z0-9_\-:.]+$/;
function validateRoomId(roomId) {
	if (typeof roomId !== 'string') return false;
	if (roomId.length === 0 || roomId.length > config.roomIdMaxLength) return false;
	return ROOM_ID_REGEX.test(roomId);
}

function safeClientId() {
	if (crypto.randomUUID) return `client:${crypto.randomUUID()}`;
	return `client:${Date.now()}:${crypto.randomBytes(8).toString('hex')}`;
}

function secureEqual(a, b) {
	if (typeof a !== 'string' || typeof b !== 'string') return false;
	const left = Buffer.from(a);
	const right = Buffer.from(b);
	return left.length === right.length && crypto.timingSafeEqual(left, right);
}

function shouldRoute(senderRole, senderSessionId, target) {
	if (!config.enableSessionAuth || senderRole === 'peer' || target.role === 'peer') return true;
	if (senderRole === target.role) return false;
	return !senderSessionId || !target.sessionId || senderSessionId === target.sessionId;
}

function handleRedisMessage(message, channel) {
	try {
		const roomId = channel.replace(/^room:/, '');
		let payload;
		try {
			payload = JSON.parse(message);
		} catch (e) {
			log('warn', 'Dropping non-JSON message from Redis', { channel, err: e });
			return;
		}
		const { senderId, senderRole, sessionId, data, originServerId } = payload || {};
		if (originServerId && originServerId === serverInstanceId) {
			return;
		}
		const clientsInRoom = localRooms.get(roomId);
		if (!clientsInRoom || clientsInRoom.size === 0) return;
		const dataStr = JSON.stringify(data);
		const endFanout = startFanoutTimer();
		clientsInRoom.forEach(client => {
			if (client.clientId !== senderId && client.readyState === WebSocket.OPEN &&
				shouldRoute(senderRole || 'peer', sessionId, client)) {
				if (client.bufferedAmount > config.backpressureCloseThresholdBytes) {
					log('warn', 'Closing client due to excessive backpressure', { clientId: client.clientId, roomId });
					incBackpressureCloses();
					try { client.close(1013, 'Server overloaded'); } catch (_) {}
					return;
				}
				try { client.send(dataStr); } catch (e) {
					log('warn', 'Failed to forward message to client', { clientId: client.clientId, roomId, err: e });
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
	const setupCloseHandler = () => { closedDuringSetup = true; };
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
		const parameters = new url.URL(request.url, `ws://${request.headers.host}`).searchParams;
		const roomId = parameters.get('roomId');
		const role = parameters.get('role') || 'peer';
		if (!validateRoomId(roomId)) {
			log('warn', 'Invalid or missing roomId on connection', { roomId });
			ws.close(1008, 'Invalid roomId');
			return;
		}
		if (!['host', 'player', 'peer'].includes(role) || (config.enableSessionAuth && role === 'peer')) {
			ws.close(1008, 'Invalid role');
			return;
		}

		let authenticatedSessionId;
		let pairingClaims;
		if (config.enableSessionAuth) {
			const authHeader = request.headers['authorization'] || '';
			const bearer = authHeader.toLowerCase().startsWith('bearer ') ? authHeader.slice(7) : '';
			if (role === 'host') {
				if (!secureEqual(bearer, config.hostSecret)) {
					ws.close(1008, 'Unauthorized host');
					return;
				}
			} else {
				pairingClaims = verifyPairingToken(parameters.get('token'), config.pairingTokenSecret);
				if (!pairingClaims || pairingClaims.roomId !== roomId) {
					ws.close(1008, 'Invalid or expired pairing token');
					return;
				}
				authenticatedSessionId = pairingClaims.sessionId;
			}
		}
		const origin = request.headers['origin'];
		const protocols = request.headers['sec-websocket-protocol'];
		if (config.requireWss && request.headers['x-forwarded-proto'] !== 'https') {
			log('warn', 'Rejected non-WSS connection in production');
			ws.close(1008, 'WSS required');
			return;
		}
		if (config.allowedOrigins.length > 0) {
			const allowed = (role === 'host' && !origin) || config.allowedOrigins.some((o) => {
				if (origin === o) return true;
				try {
					const originUrl = new URL(origin);
					const allowedUrl = o.startsWith('http://') || o.startsWith('https://') 
						? new URL(o) 
						: new URL(`http://${o}`);
					return originUrl.host === allowedUrl.host || originUrl.hostname === allowedUrl.hostname;
				} catch {
					return false;
				}
			});
			if (!allowed) {
				log('warn', 'Origin not allowed', { origin, allowedOrigins: config.allowedOrigins });
				ws.close(1008, 'Origin not allowed');
				return;
			}
		}
		if (config.subprotocol) {
			const hasProto = protocols && protocols.split(',').map(s => s.trim()).includes(config.subprotocol);
			if (!hasProto) {
				log('warn', 'Missing required subprotocol');
				ws.close(1008, 'Subprotocol required');
				return;
			}
			try { ws.protocol = config.subprotocol; } catch (_) {}
		}
		if (config.enableAuth) {
			const authHeader = request.headers['authorization'] || '';
			let token = parameters.get(config.enableSessionAuth ? 'accessToken' : 'token');
			if (!token && authHeader.toLowerCase().startsWith('bearer ')) token = authHeader.slice(7);
			if (!token) {
				log('warn', 'Missing JWT');
				ws.close(1008, 'Unauthorized');
				return;
			}
			try {
				let payload;
				if (config.jwt.jwksUrl) {
					const { payload: pl } = await jwtVerify(token, getJwks(), { issuer: config.jwt.issuer, audience: config.jwt.audience });
					payload = pl;
				} else {
					payload = jwt.verify(token, config.jwt.secret, { algorithms: [config.jwt.alg], issuer: config.jwt.issuer, audience: config.jwt.audience });
				}
				const allowedRooms = payload[config.jwt.roomsClaim];
				if (Array.isArray(allowedRooms) && !allowedRooms.includes(roomId)) {
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

		const ip = (request.socket && request.socket.remoteAddress) || 'unknown';
		let allowedConn = true;
		try {
			allowedConn = await rateLimiter.allow({ namespace: 'conn', id: ip, limit: config.rateLimitConnPer10s, periodSeconds: 10 });
		} catch (e) {
			log('warn', 'Rate limiter error on connection', { ip, err: e });
		}
		if (!allowedConn) {
			log('warn', 'IP connection rate-limited', { ip });
			ws.close(1013, 'Rate limited');
			return;
		}
		if (pairingClaims) {
			try {
				const ttlMs = Math.max(1, pairingClaims.exp - Date.now());
				const claimed = await redisClient.set(`pairing-used:${pairingClaims.jti}`, '1', { NX: true, PX: ttlMs });
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

		const roomKey = `room:${roomId}`;
		const clientId = safeClientId();

		try {
			const end = startRedisTimer();
			const result = await atomicJoin(redisClient, roomKey, clientId, config.roomCapacity);
			end();
			noteRedisSuccess();
			if (result === -1) {
				log('info', 'Room is full, rejecting connection', { roomId, clientId });
				ws.close(1000, 'Room is full');
				return;
			}
		} catch (e) {
			log('error', 'Redis error during join', { roomId, clientId, err: e });
			noteRedisFailure();
			ws.close(1011, 'Internal error');
			return;
		}
		if (closedDuringSetup || ws.readyState !== WebSocket.OPEN) {
			try { await atomicLeave(redisClient, roomKey, clientId, config.roomTtlSeconds); } catch (_) {}
			return;
		}

		ws.roomId = roomId;
		ws.clientId = clientId;
		ws.role = role;
		ws.sessionId = authenticatedSessionId;
		ws.isAlive = true;
		ws._rate = { tokens: config.rateLimitMessagesPer10s, lastRefill: Date.now() };

		if (!localRooms.has(roomId)) localRooms.set(roomId, new Set());
		localRooms.get(roomId).add(ws);
		localConnectionCount++;
		setActiveConnections(localConnectionCount);
		setLocalRooms(localRooms.size);

		log('info', 'Client joined room', { clientId, roomId, localCount: localRooms.get(roomId).size });
		if (pairingClaims && ws.readyState === WebSocket.OPEN) {
			const nextToken = signPairingToken({ roomId, sessionId: authenticatedSessionId,
				expiresAt: Date.now() + config.pairingTokenTtlSeconds * 1000 }, config.pairingTokenSecret);
			ws.send(JSON.stringify({ type: 'control', sessionId: authenticatedSessionId,
				action: 'session-ready', payload: { pairingToken: nextToken } }));
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
				try { ws.close(1013, 'Too many pending messages'); } catch (_) {}
				return;
			}
			ws._pendingMessages++;
			ws._messageChain = ws._messageChain
				.then(() => handleMessage(ws, roomKey, message))
				.catch((err) => log('error', 'Queued message handler failed', { clientId, roomId, err }))
				.finally(() => { ws._pendingMessages--; });
		});
		ws.off('close', setupCloseHandler);
		ws.on('close', () => handleDisconnection(ws, roomKey));
		ws.on('error', (err) => {
			log('warn', 'WebSocket error', { clientId: ws.clientId, roomId: ws.roomId, err });
		});
	} catch (error) {
		log('error', 'Unhandled error during connection setup', { err: error });
		try { ws.close(1011, 'Internal server error'); } catch (_) {}
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
				log('warn', 'Dropping oversized text message', { clientId: ws.clientId, roomId: ws.roomId });
				return;
			}
		} else if (Buffer.isBuffer(message)) {
			if (message.length > config.messageMaxBytes) {
				log('warn', 'Dropping oversized binary message', { clientId: ws.clientId, roomId: ws.roomId });
				return;
			}
			message = message.toString('utf8');
		} else {
			return;
		}

		// Rate limiting
		refillTokens(ws._rate);
		if (ws._rate.tokens <= 0) {
			log('warn', 'Rate limit exceeded, dropping message', { clientId: ws.clientId, roomId: ws.roomId });
			return;
		}
		ws._rate.tokens -= 1;

		// Backpressure check
		if (ws.bufferedAmount > config.backpressureCloseThresholdBytes) {
			log('warn', 'Closing client due to excessive backpressure (sender)', { clientId: ws.clientId, roomId: ws.roomId });
			try { ws.close(1013, 'Server overloaded'); } catch (_) {}
			return;
		}

		let parsedMessage;
		try { parsedMessage = JSON.parse(message); } catch (e) {
			log('warn', 'Dropping non-JSON client message', { clientId: ws.clientId, roomId: ws.roomId });
			return;
		}

		// Schema validation
		const validation = validateSignalingMessage(parsedMessage);
		if (!validation.ok) {
			log('warn', 'Dropping invalid signaling message', { clientId: ws.clientId, roomId: ws.roomId });
			// Optionally send a control error
			try { ws.send(JSON.stringify({ type: 'control', action: 'schema-error' })); } catch (_) {}
			incSchemaRejects();
			return;
		}
		const messageSessionId = validation.data.sessionId;
		if (config.enableSessionAuth) {
			if (!messageSessionId) {
				log('warn', 'Dropping signaling message without sessionId', { clientId: ws.clientId, roomId: ws.roomId });
				return;
			}
			if (ws.role === 'player' && messageSessionId !== ws.sessionId) {
				log('warn', 'Dropping stale or replayed player message', { clientId: ws.clientId, roomId: ws.roomId });
				return;
			}
			if (ws.role === 'host') {
				const peers = localRooms.get(ws.roomId);
				const known = peers && [...peers].some(peer => peer.role === 'player' && peer.sessionId === messageSessionId);
				if (!known) return;
			}
		}

		// IP and room message rate limits
		const ip = (ws._socket && ws._socket.remoteAddress) || 'unknown';
		let allowedMsg = true;
		try {
			allowedMsg = await rateLimiter.allow({ namespace: 'msg-ip', id: ip, limit: config.rateLimitIpMsgsPer10s, periodSeconds: 10 });
			if (allowedMsg) {
				allowedMsg = await rateLimiter.allow({ namespace: 'msg-room', id: ws.roomId, limit: config.rateLimitRoomMsgsPer10s, periodSeconds: 10 });
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
					if (peer !== ws && peer.readyState === WebSocket.OPEN &&
						shouldRoute(ws.role, messageSessionId, peer)) {
						if (peer.bufferedAmount <= config.backpressureCloseThresholdBytes) {
							try { peer.send(payload); } catch (_) {}
						}
					}
				});
			}
		} catch (_) {}

		try {
			const ePub = startRedisTimer();
			await redisClient.publish(roomKey, JSON.stringify({ senderId: ws.clientId, senderRole: ws.role,
				sessionId: messageSessionId, data: validation.data, originServerId: serverInstanceId }));
			ePub();
			noteRedisSuccess();
		} catch (e) {
			log('error', 'Failed to publish to Redis', { roomId: ws.roomId, clientId: ws.clientId, err: e });
			noteRedisFailure();
		}
	} catch (error) {
		log('error', 'Unhandled error in message handler', { clientId: ws.clientId, roomId: ws.roomId, err: error });
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
		await atomicLeave(redisClient, roomKey, clientId, config.roomTtlSeconds);
		end();
		noteRedisSuccess();
		await redisClient.publish(roomKey, JSON.stringify({ senderId: clientId, senderRole: ws.role,
			sessionId: ws.sessionId, data: { type: 'peer-disconnected', sessionId: ws.sessionId } }));
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
		await subscriber.pSubscribe('room:*', handleRedisMessage);
		log('info', 'Subscribed to Redis channel pattern', { pattern: 'room:*' });
	} catch (e) {
		log('error', 'Failed to subscribe to Redis pattern', { err: e });
		process.exit(1);
	}

	server.on('connection', (ws, request) => {
		if (draining) {
			try { ws.close(config.shutdownCloseCode, 'Server draining'); } catch (_) {}
			return;
		}
		handleNewConnection(ws, request);
	});
	server.on('error', (err) => log('error', 'WebSocket server error', { err }));
	heartbeatTimer = setInterval(() => {
		server.clients.forEach((ws) => {
			if (ws.readyState !== WebSocket.OPEN) return;
			if (!ws.isAlive) {
				log('warn', 'Terminating unresponsive client', { clientId: ws.clientId, roomId: ws.roomId });
				try { ws.terminate(); } catch (_) {}
				return;
			}
			ws.isAlive = false;
			try { ws.ping(); } catch (e) {
				log('warn', 'Failed to send ping', { clientId: ws.clientId, err: e });
			}
		});
	}, config.heartbeatIntervalMs);
	if (typeof heartbeatTimer.unref === 'function') heartbeatTimer.unref();

	// Start the combined HTTP+WS server on Railway's injected PORT (or local fallback)
	const listenPort = process.env.PORT || config.wsPort;
	httpServer.listen(listenPort, () => {
		log('info', 'Scalable Signaling Server listening', { port: listenPort });
	});
}

main().catch(err => {
	log('error', 'Unhandled error in main()', { err });
	process.exit(1);
});

// Graceful shutdown
async function shutdown() {
	log('info', 'Shutting down gracefully...');
	// Enter drain mode
	draining = true;
	if (heartbeatTimer) clearInterval(heartbeatTimer);
	try { server.close(); } catch (_) {}

	// Best-effort close of all clients and Redis membership cleanup, and wait for close frames to flush
	const closePromises = [];
	localRooms.forEach((clients, roomId) => {
		clients.forEach((ws) => {
			try { ws.close(config.shutdownCloseCode, 'Server draining'); } catch (_) {}
			const roomKey = `room:${roomId}`;
			const waitForClose = new Promise((resolve) => {
				let resolved = false;
				const done = () => { if (!resolved) { resolved = true; resolve(); } };
				try { ws.once('close', () => done()); } catch (_) { done(); }
				// Fallback in case close event doesn't arrive in time
				setTimeout(done, Math.min(1000, config.drainTimeoutMs));
			});
			closePromises.push((async () => {
				try { let e1 = startRedisTimer(); await redisClient.sRem(roomKey, ws.clientId); e1(); } catch (_) {}
				try { let e2 = startRedisTimer(); await redisClient.publish(roomKey, JSON.stringify({ senderId: ws.clientId, data: { type: 'peer-disconnected' } })); e2(); } catch (_) {}
				await waitForClose;
			})());
		});
	});

	try {
		await Promise.race([
			Promise.all(closePromises).catch(() => {}),
			new Promise((resolve) => setTimeout(resolve, config.drainTimeoutMs)),
		]);
	} catch (_) {}

	try { await subscriber.quit(); } catch (_) {}
	try { await redisClient.quit(); } catch (_) {}
	process.exit(0);
}

process.on('SIGINT', shutdown);
process.on('SIGTERM', shutdown);
