const { z } = require('zod');
const path = require('path');
require('dotenv').config({ path: path.resolve(__dirname, '.env') });

const schema = z.object({
	NODE_ENV: z.string().default(process.env.NODE_ENV || 'development'),
	WS_PORT: z.preprocess((v) => Number(v), z.number().int().positive()).default(3002),
	MATCHMAKER_PORT: z.preprocess((v) => Number(v), z.number().int().positive()).default(3000),
	REDIS_URL: z.string().url().default('redis://127.0.0.1:6379'),
	ROOM_CAPACITY: z.preprocess((v) => Number(v), z.number().int().positive()).default(2),
	ROOM_TTL_SECONDS: z.preprocess((v) => Number(v), z.number().int().positive()).default(120),
	HOST_TTL_SECONDS: z.preprocess((v) => Number(v), z.number().int().positive()).default(60),
	ALLOCATION_RESERVATION_SECONDS: z.preprocess((v) => Number(v), z.number().int().positive()).default(20),
	MESSAGE_MAX_BYTES: z.preprocess((v) => Number(v), z.number().int().positive()).default(256 * 1024),
	BACKPRESSURE_CLOSE_THRESHOLD_BYTES: z.preprocess((v) => Number(v), z.number().int().positive()).default(5 * 1024 * 1024),
	HEARTBEAT_INTERVAL_MS: z.preprocess((v) => Number(v), z.number().int().positive()).default(30000),
	RATE_LIMIT_MESSAGES_PER_10S: z.preprocess((v) => Number(v), z.number().int().positive()).default(200),
	ROOM_ID_MAX_LENGTH: z.preprocess((v) => Number(v), z.number().int().positive()).default(64),
	HEALTH_PORT: z.preprocess((v) => Number(v), z.number().int().positive()).default(8080),
	LOG_LEVEL: z.string().default(process.env.LOG_LEVEL || 'info'),
	PRETTY_LOGS: z.string().optional(),
	RATE_LIMIT_CONN_PER_10S: z.preprocess((v) => Number(v), z.number().int().positive()).default(20),
	RATE_LIMIT_IP_MSGS_PER_10S: z.preprocess((v) => Number(v), z.number().int().positive()).default(400),
	RATE_LIMIT_ROOM_MSGS_PER_10S: z.preprocess((v) => Number(v), z.number().int().positive()).default(800),
	DRAIN_TIMEOUT_MS: z.preprocess((v) => Number(v), z.number().int().positive()).default(5000),
	SHUTDOWN_CLOSE_CODE: z.preprocess((v) => Number(v), z.number().int().positive()).default(1012),
	CB_ERROR_THRESHOLD: z.preprocess((v) => Number(v), z.number().int().positive()).default(5),
	CB_OPEN_MS: z.preprocess((v) => Number(v), z.number().int().positive()).default(10000),
	REQUIRE_WSS: z.string().optional(),
	ALLOWED_ORIGINS: z.string().optional(),
	SUBPROTOCOL: z.string().optional(),
	HOST_SECRET: z.string().min(1).optional(),
	HOST_SECRET_PREVIOUS: z.string().optional(),
	ENABLE_AUTH: z.string().optional(),
	ENABLE_SESSION_AUTH: z.string().optional(),
	PAIRING_TOKEN_SECRET: z.string().min(1).optional(),
	PAIRING_TOKEN_TTL_SECONDS: z.preprocess((v) => Number(v), z.number().int().min(30).max(600)).default(120),
	JWT_ISSUER: z.string().optional(),
	JWT_AUDIENCE: z.string().optional(),
	JWT_ALG: z.string().optional(),
	JWT_SECRET: z.string().optional(),
	JWKS_URL: z.string().optional(),
	JWKS_CACHE_TTL: z.preprocess((v) => Number(v), z.number().int().positive()).optional(),
	ROOMS_CLAIM: z.string().optional(),
	METERED_DOMAIN:  z.string().optional(),
	METERED_API_KEY: z.string().optional(),
	TURN_EXPIRY_SECONDS: z.preprocess((v) => Number(v), z.number().int().positive()).default(14400), // 4 hours
});

let parsed;
try {
	parsed = schema.parse(process.env);
} catch (e) {
	// Minimal console since logger might not be ready
	console.error('[config] Invalid configuration:', e.errors || e.message || String(e));
	process.exit(1);
}

const config = {
	env: parsed.NODE_ENV,
	wsPort: parsed.WS_PORT,
	mmPort: parsed.MATCHMAKER_PORT,
	redisUrl: parsed.REDIS_URL,
	roomCapacity: parsed.ROOM_CAPACITY,
	roomTtlSeconds: parsed.ROOM_TTL_SECONDS,
	hostTtlSeconds: parsed.HOST_TTL_SECONDS,
	allocationReservationSeconds: parsed.ALLOCATION_RESERVATION_SECONDS,
	messageMaxBytes: parsed.MESSAGE_MAX_BYTES,
	backpressureCloseThresholdBytes: parsed.BACKPRESSURE_CLOSE_THRESHOLD_BYTES,
	heartbeatIntervalMs: parsed.HEARTBEAT_INTERVAL_MS,
	rateLimitMessagesPer10s: parsed.RATE_LIMIT_MESSAGES_PER_10S,
	rateLimitConnPer10s: parsed.RATE_LIMIT_CONN_PER_10S,
	rateLimitIpMsgsPer10s: parsed.RATE_LIMIT_IP_MSGS_PER_10S,
	rateLimitRoomMsgsPer10s: parsed.RATE_LIMIT_ROOM_MSGS_PER_10S,
	roomIdMaxLength: parsed.ROOM_ID_MAX_LENGTH,
	healthPort: parsed.HEALTH_PORT,
	logLevel: parsed.LOG_LEVEL,
	prettyLogs: parsed.PRETTY_LOGS === 'true',
	drainTimeoutMs: parsed.DRAIN_TIMEOUT_MS,
	shutdownCloseCode: parsed.SHUTDOWN_CLOSE_CODE,
	cbErrorThreshold: parsed.CB_ERROR_THRESHOLD,
	cbOpenMs: parsed.CB_OPEN_MS,
	requireWss: parsed.REQUIRE_WSS === 'true',
	allowedOrigins: (parsed.ALLOWED_ORIGINS || '').split(',').map(s => s.trim()).filter(Boolean),
	subprotocol: parsed.SUBPROTOCOL,
	hostSecret: parsed.HOST_SECRET,
	hostSecretPrevious: parsed.HOST_SECRET_PREVIOUS,
	enableAuth: parsed.ENABLE_AUTH === 'true',
	enableSessionAuth: parsed.ENABLE_SESSION_AUTH === 'true' || parsed.NODE_ENV === 'production',
	pairingTokenSecret: parsed.PAIRING_TOKEN_SECRET,
	pairingTokenTtlSeconds: parsed.PAIRING_TOKEN_TTL_SECONDS,
	jwt: {
		issuer: parsed.JWT_ISSUER,
		audience: parsed.JWT_AUDIENCE,
		alg: parsed.JWT_ALG || 'HS256',
		secret: parsed.JWT_SECRET,
		jwksUrl: parsed.JWKS_URL,
		jwksTtlMs: parsed.JWKS_CACHE_TTL || 300000,
		roomsClaim: parsed.ROOMS_CLAIM || 'rooms',
	},
	metered: {
		domain:          parsed.METERED_DOMAIN  || null,
		apiKey:          parsed.METERED_API_KEY || null,
		expirySeconds:   parsed.TURN_EXPIRY_SECONDS,
	},
};

if (config.env === 'production') {
	const missing = [];
	if (!config.requireWss) missing.push('REQUIRE_WSS=true');
	if (!config.hostSecret || config.hostSecret.length < 32) missing.push('HOST_SECRET (at least 32 characters)');
	if (!config.pairingTokenSecret || config.pairingTokenSecret.length < 32) missing.push('PAIRING_TOKEN_SECRET (at least 32 characters)');
	if (missing.length) throw new Error(`Production security configuration missing: ${missing.join(', ')}`);
}

module.exports = { config };


