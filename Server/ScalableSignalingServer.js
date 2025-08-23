const WebSocket = require('ws');
const url = require('url');
const crypto = require('crypto');
const { createClient } = require('redis');

// =============================
// Configuration (env-driven)
// =============================
const envInt = (name, def) => {
	const raw = process.env[name];
	if (!raw) return def;
	const n = Number(raw);
	return Number.isFinite(n) ? n : def;
};

const WEBSOCKET_PORT = envInt('WS_PORT', 3002);
const REDIS_URL = process.env.REDIS_URL || 'redis://127.0.0.1:6379';
const ROOM_CAPACITY = envInt('ROOM_CAPACITY', 2);
const ROOM_TTL_SECONDS = envInt('ROOM_TTL_SECONDS', 120);
const MESSAGE_MAX_BYTES = envInt('MESSAGE_MAX_BYTES', 256 * 1024); // 256KB
const BACKPRESSURE_CLOSE_THRESHOLD_BYTES = envInt('BACKPRESSURE_CLOSE_THRESHOLD_BYTES', 5 * 1024 * 1024); // 5MB
const HEARTBEAT_INTERVAL_MS = envInt('HEARTBEAT_INTERVAL_MS', 15000);
const RATE_LIMIT_MESSAGES_PER_10S = envInt('RATE_LIMIT_MESSAGES_PER_10S', 200);
const ROOM_ID_MAX_LENGTH = envInt('ROOM_ID_MAX_LENGTH', 64);

// =============================
// Logging helpers
// =============================
function log(level, message, context) {
	const base = { level, time: new Date().toISOString(), message };
	console[level === 'error' ? 'error' : level === 'warn' ? 'warn' : 'log'](
		JSON.stringify(context ? { ...base, ...context } : base)
	);
}

// =============================
// Redis Clients and helpers
// =============================
function createRedis(urlString) {
	return createClient({
		url: urlString,
		socket: {
			reconnectStrategy: (retries) => {
				const delay = Math.min(1000 + retries * 50, 5000);
				return delay; // retry with capped backoff
			}
		}
	});
}

// Need two clients: one for commands, one for subscriber mode
const redisClient = createRedis(REDIS_URL);
const subscriber = redisClient.duplicate();

redisClient.on('error', (err) => log('error', 'Redis client error', { err: String(err && err.message || err) }));
subscriber.on('error', (err) => log('error', 'Redis subscriber error', { err: String(err && err.message || err) }));

// =============================
// WebSocket Server
// =============================
const server = new WebSocket.Server({ port: WEBSOCKET_PORT, maxPayload: MESSAGE_MAX_BYTES });

// In-memory map for clients connected to THIS server instance.
// Map<roomId, Set<WebSocket>>
const localRooms = new Map();

// =============================
// Validation helpers
// =============================
const ROOM_ID_REGEX = /^[A-Za-z0-9_\-:.]+$/;
function validateRoomId(roomId) {
	if (typeof roomId !== 'string') return false;
	if (roomId.length === 0 || roomId.length > ROOM_ID_MAX_LENGTH) return false;
	return ROOM_ID_REGEX.test(roomId);
}

function safeClientId() {
	if (crypto.randomUUID) return `client:${crypto.randomUUID()}`;
	return `client:${Date.now()}:${crypto.randomBytes(8).toString('hex')}`;
}

// =============================
// Redis Pub/Sub handler
// =============================
function handleRedisMessage(message, channel) {
	try {
		const roomId = channel.replace(/^room:/, '');
		let payload;
		try {
			payload = JSON.parse(message);
		} catch (e) {
			log('warn', 'Dropping non-JSON message from Redis', { channel, error: String(e && e.message || e) });
			return;
		}
		const { senderId, data } = payload || {};
		const clientsInRoom = localRooms.get(roomId);
		if (!clientsInRoom || clientsInRoom.size === 0) return;
		const dataStr = JSON.stringify(data);
		clientsInRoom.forEach(client => {
			if (client.clientId !== senderId && client.readyState === WebSocket.OPEN) {
				// Backpressure guard
				if (client.bufferedAmount > BACKPRESSURE_CLOSE_THRESHOLD_BYTES) {
					log('warn', 'Closing client due to excessive backpressure', { clientId: client.clientId, roomId });
					try { client.close(1013, 'Server overloaded'); } catch (_) {}
					return;
				}
				try { client.send(dataStr); } catch (e) {
					log('warn', 'Failed to forward message to client', { clientId: client.clientId, roomId, error: String(e && e.message || e) });
				}
			}
		});
	} catch (error) {
		log('error', 'Error handling Redis message', { channel, error: String(error && error.message || error) });
	}
}

// =============================
// Connection lifecycle handlers
// =============================
async function handleNewConnection(ws, request) {
	try {
		if (!request || !request.url || !request.headers || !request.headers.host) {
			log('warn', 'Malformed connection request');
			ws.close(1008, 'Malformed request');
			return;
		}
		const parameters = new url.URL(request.url, `ws://${request.headers.host}`).searchParams;
		const roomId = parameters.get('roomId');
		if (!validateRoomId(roomId)) {
			log('warn', 'Invalid or missing roomId on connection', { roomId });
			ws.close(1008, 'Invalid roomId');
			return;
		}

		const roomKey = `room:${roomId}`;
		const clientId = safeClientId();

		// Attempt to join atomically-ish: add first, then check size and roll back if over capacity
		let joined = false;
		try {
			await redisClient.sAdd(roomKey, clientId);
			const size = await redisClient.sCard(roomKey);
			if (size > ROOM_CAPACITY) {
				await redisClient.sRem(roomKey, clientId);
				log('info', 'Room is full, rejecting connection', { roomId, clientId, size });
				ws.close(1000, 'Room is full');
				return;
			}
			joined = true;
			if (size === 1) {
				// Room created (first member) -> remove TTL
				try { await redisClient.persist(roomKey); } catch (e) { log('warn', 'Failed to persist room key', { roomId, error: String(e && e.message || e) }); }
			}
		} catch (e) {
			log('error', 'Redis error during join', { roomId, clientId, error: String(e && e.message || e) });
			if (joined) { try { await redisClient.sRem(roomKey, clientId); } catch (_) {} }
			ws.close(1011, 'Internal error');
			return;
		}

		// Attach metadata
		ws.roomId = roomId;
		ws.clientId = clientId;
		ws.isAlive = true;
		ws._rate = { tokens: RATE_LIMIT_MESSAGES_PER_10S, lastRefill: Date.now() };

		// Register locally
		if (!localRooms.has(roomId)) localRooms.set(roomId, new Set());
		localRooms.get(roomId).add(ws);

		log('info', 'Client joined room', { clientId, roomId, localCount: localRooms.get(roomId).size });

		// Heartbeat
		const heartbeat = setInterval(() => {
			if (!ws || ws.readyState !== WebSocket.OPEN) return;
			if (!ws.isAlive) {
				log('warn', 'Terminating unresponsive client', { clientId: ws.clientId, roomId: ws.roomId });
				try { ws.terminate(); } catch (_) {}
				return;
			}
			ws.isAlive = false;
			try { ws.ping(); } catch (_) {}
		}, HEARTBEAT_INTERVAL_MS);
		ws._heartbeat = heartbeat;
		ws.on('pong', () => { ws.isAlive = true; });

		// Wire message / close / error handlers
		ws.on('message', (message) => handleMessage(ws, roomKey, message));
		ws.on('close', () => handleDisconnection(ws, roomKey));
		ws.on('error', (err) => {
			log('warn', 'WebSocket error', { clientId: ws.clientId, roomId: ws.roomId, error: String(err && err.message || err) });
		});
	} catch (error) {
		log('error', 'Unhandled error during connection setup', { error: String(error && error.message || error) });
		try { ws.close(1011, 'Internal server error'); } catch (_) {}
	}
}

function refillTokens(rate) {
	const now = Date.now();
	const elapsed = now - rate.lastRefill;
	if (elapsed <= 0) return;
	const tokensToAdd = Math.floor((RATE_LIMIT_MESSAGES_PER_10S / 10000) * elapsed);
	rate.tokens = Math.min(RATE_LIMIT_MESSAGES_PER_10S, rate.tokens + tokensToAdd);
	rate.lastRefill = now;
}

async function handleMessage(ws, roomKey, message) {
	try {
		if (typeof message === 'string') {
			if (Buffer.byteLength(message) > MESSAGE_MAX_BYTES) {
				log('warn', 'Dropping oversized text message', { clientId: ws.clientId, roomId: ws.roomId });
				return;
			}
		} else if (Buffer.isBuffer(message)) {
			if (message.length > MESSAGE_MAX_BYTES) {
				log('warn', 'Dropping oversized binary message', { clientId: ws.clientId, roomId: ws.roomId });
				return;
			}
			message = message.toString('utf8');
		} else {
			// Unsupported frame type
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
		if (ws.bufferedAmount > BACKPRESSURE_CLOSE_THRESHOLD_BYTES) {
			log('warn', 'Closing client due to excessive backpressure (sender)', { clientId: ws.clientId, roomId: ws.roomId });
			try { ws.close(1013, 'Server overloaded'); } catch (_) {}
			return;
		}

		let parsedMessage;
		try { parsedMessage = JSON.parse(message); } catch (e) {
			log('warn', 'Dropping non-JSON client message', { clientId: ws.clientId, roomId: ws.roomId });
			return;
		}

		try {
			await redisClient.publish(roomKey, JSON.stringify({ senderId: ws.clientId, data: parsedMessage }));
		} catch (e) {
			log('error', 'Failed to publish to Redis', { roomId: ws.roomId, clientId: ws.clientId, error: String(e && e.message || e) });
		}
	} catch (error) {
		log('error', 'Unhandled error in message handler', { clientId: ws.clientId, roomId: ws.roomId, error: String(error && error.message || error) });
	}
}

async function handleDisconnection(ws, roomKey) {
	// Ensure this handler is idempotent
	try { if (ws._heartbeat) clearInterval(ws._heartbeat); } catch (_) {}
	const roomId = ws.roomId;
	const clientId = ws.clientId;
	log('info', 'Client disconnected', { clientId, roomId });

	// Remove from local map
	try {
		const roomClients = localRooms.get(roomId);
		if (roomClients) {
			roomClients.delete(ws);
			if (roomClients.size === 0) {
				localRooms.delete(roomId);
				log('info', 'Room now empty on this instance', { roomId });
			}
		}
	} catch (e) {
		log('warn', 'Local room cleanup failed', { roomId, error: String(e && e.message || e) });
	}

	// Redis cleanup and notify peers
	try {
		await redisClient.sRem(roomKey, clientId);
		await redisClient.publish(roomKey, JSON.stringify({ senderId: clientId, data: { type: 'peer-disconnected' } }));
	} catch (e) {
		log('warn', 'Redis cleanup or notify failed', { roomId, clientId, error: String(e && e.message || e) });
	}

	// If room is empty globally, set TTL
	try {
		const remaining = await redisClient.sCard(roomKey);
		if (remaining === 0) {
			try { await redisClient.expire(roomKey, ROOM_TTL_SECONDS); } catch (_) {}
			log('info', 'Room empty globally; TTL set', { roomId, ttlSeconds: ROOM_TTL_SECONDS });
		}
	} catch (e) {
		log('warn', 'Failed checking/setting room TTL', { roomId, error: String(e && e.message || e) });
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
	} catch (e) {
		log('error', 'Failed to connect to Redis', { error: String(e && e.message || e) });
		process.exit(1);
	}

	try {
		await subscriber.pSubscribe('room:*', handleRedisMessage);
		log('info', 'Subscribed to Redis channel pattern', { pattern: 'room:*' });
	} catch (e) {
		log('error', 'Failed to subscribe to Redis pattern', { error: String(e && e.message || e) });
		process.exit(1);
	}

	server.on('connection', (ws, request) => handleNewConnection(ws, request));
	server.on('error', (err) => log('error', 'WebSocket server error', { error: String(err && err.message || err) }));

	log('info', 'Scalable Signaling Server listening', { port: WEBSOCKET_PORT });
}

main().catch(err => {
	log('error', 'Unhandled error in main()', { error: String(err && err.message || err) });
	process.exit(1);
});

// Graceful shutdown
async function shutdown() {
	log('info', 'Shutting down gracefully...');
	try { server.close(); } catch (_) {}
	try { await subscriber.quit(); } catch (_) {}
	try { await redisClient.quit(); } catch (_) {}
	process.exit(0);
}

process.on('SIGINT', shutdown);
process.on('SIGTERM', shutdown);
