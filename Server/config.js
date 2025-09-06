const { z } = require('zod');
require('dotenv').config();

const schema = z.object({
	NODE_ENV: z.string().default(process.env.NODE_ENV || 'development'),
	WS_PORT: z.preprocess((v) => Number(v), z.number().int().positive()).default(3002),
	REDIS_URL: z.string().url().default('redis://127.0.0.1:6379'),
	ROOM_CAPACITY: z.preprocess((v) => Number(v), z.number().int().positive()).default(2),
	ROOM_TTL_SECONDS: z.preprocess((v) => Number(v), z.number().int().positive()).default(120),
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
	ENABLE_AUTH: z.string().optional(),
	JWT_ISSUER: z.string().optional(),
	JWT_AUDIENCE: z.string().optional(),
	JWT_ALG: z.string().optional(),
	JWT_SECRET: z.string().optional(),
	JWKS_URL: z.string().optional(),
	JWKS_CACHE_TTL: z.preprocess((v) => Number(v), z.number().int().positive()).optional(),
	ROOMS_CLAIM: z.string().optional(),
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
	redisUrl: parsed.REDIS_URL,
	roomCapacity: parsed.ROOM_CAPACITY,
	roomTtlSeconds: parsed.ROOM_TTL_SECONDS,
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
	enableAuth: parsed.ENABLE_AUTH === 'true',
	jwt: {
		issuer: parsed.JWT_ISSUER,
		audience: parsed.JWT_AUDIENCE,
		alg: parsed.JWT_ALG || 'HS256',
		secret: parsed.JWT_SECRET,
		jwksUrl: parsed.JWKS_URL,
		jwksTtlMs: parsed.JWKS_CACHE_TTL || 300000,
		roomsClaim: parsed.ROOMS_CLAIM || 'rooms',
	},
};

module.exports = { config };


