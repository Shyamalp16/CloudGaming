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
	HEARTBEAT_INTERVAL_MS: z.preprocess((v) => Number(v), z.number().int().positive()).default(15000),
	RATE_LIMIT_MESSAGES_PER_10S: z.preprocess((v) => Number(v), z.number().int().positive()).default(200),
	ROOM_ID_MAX_LENGTH: z.preprocess((v) => Number(v), z.number().int().positive()).default(64),
	HEALTH_PORT: z.preprocess((v) => Number(v), z.number().int().positive()).default(8080),
	LOG_LEVEL: z.string().default(process.env.LOG_LEVEL || 'info'),
	PRETTY_LOGS: z.string().optional(),
	RATE_LIMIT_CONN_PER_10S: z.preprocess((v) => Number(v), z.number().int().positive()).default(20),
	RATE_LIMIT_IP_MSGS_PER_10S: z.preprocess((v) => Number(v), z.number().int().positive()).default(400),
	RATE_LIMIT_ROOM_MSGS_PER_10S: z.preprocess((v) => Number(v), z.number().int().positive()).default(800),
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
};

module.exports = { config };


