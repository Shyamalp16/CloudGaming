const { z } = require('zod');
const path = require('path');
const { canonicalOrigin, UUID_PATTERN } = require('./security');
const { isIP } = require('net');
if (process.env.NODE_ENV !== 'test' && process.env.JEST_WORKER_ID === undefined) {
  require('dotenv').config({ path: path.resolve(__dirname, '.env') });
}

const schema = z.object({
  NODE_ENV: z.string().default(process.env.NODE_ENV || 'development'),
  WS_PORT: z.preprocess((v) => Number(v), z.number().int().min(1).max(65535)).default(3002),
  MATCHMAKER_PORT: z.preprocess((v) => Number(v), z.number().int().min(1).max(65535)).default(3000),
  BIND_HOST: z.string().min(1).max(255).default('127.0.0.1'),
  TRUSTED_PROXY_IPS: z.string().optional(),
  REDIS_URL: z.string().url().default('redis://127.0.0.1:6379'),
  REDIS_KEY_PREFIX: z
    .string()
    .regex(/^[A-Za-z0-9:_-]{1,32}$/)
    .default('cg:v1:'),
  ROOM_CAPACITY: z.preprocess((v) => Number(v), z.number().int().min(2).max(4)).default(2),
  ROOM_TTL_SECONDS: z.preprocess((v) => Number(v), z.number().int().min(30).max(3600)).default(120),
  HOST_TTL_SECONDS: z.preprocess((v) => Number(v), z.number().int().min(30).max(600)).default(60),
  ALLOCATION_RESERVATION_SECONDS: z
    .preprocess((v) => Number(v), z.number().int().min(5).max(120))
    .default(20),
  MESSAGE_MAX_BYTES: z
    .preprocess(
      (v) => Number(v),
      z
        .number()
        .int()
        .min(1024)
        .max(1024 * 1024),
    )
    .default(256 * 1024),
  BACKPRESSURE_CLOSE_THRESHOLD_BYTES: z
    .preprocess(
      (v) => Number(v),
      z
        .number()
        .int()
        .min(64 * 1024)
        .max(16 * 1024 * 1024),
    )
    .default(5 * 1024 * 1024),
  HEARTBEAT_INTERVAL_MS: z
    .preprocess((v) => Number(v), z.number().int().min(5000).max(120000))
    .default(30000),
  RATE_LIMIT_MESSAGES_PER_10S: z
    .preprocess((v) => Number(v), z.number().int().min(1).max(10000))
    .default(200),
  ROOM_ID_MAX_LENGTH: z.preprocess((v) => Number(v), z.number().int().min(32).max(32)).default(32),
  HEALTH_PORT: z.preprocess((v) => Number(v), z.number().int().min(1).max(65535)).default(8081),
  LOG_LEVEL: z
    .enum(['fatal', 'error', 'warn', 'info', 'debug', 'trace', 'silent'])
    .default(process.env.LOG_LEVEL || 'info'),
  PRETTY_LOGS: z.string().optional(),
  RATE_LIMIT_CONN_PER_10S: z
    .preprocess((v) => Number(v), z.number().int().min(1).max(1000))
    .default(20),
  RATE_LIMIT_IP_MSGS_PER_10S: z
    .preprocess((v) => Number(v), z.number().int().min(1).max(20000))
    .default(400),
  RATE_LIMIT_ROOM_MSGS_PER_10S: z
    .preprocess((v) => Number(v), z.number().int().min(1).max(40000))
    .default(800),
  DRAIN_TIMEOUT_MS: z
    .preprocess((v) => Number(v), z.number().int().min(1000).max(30000))
    .default(5000),
  SHUTDOWN_CLOSE_CODE: z
    .preprocess((v) => Number(v), z.number().int().min(1000).max(4999))
    .default(1012),
  CB_ERROR_THRESHOLD: z.preprocess((v) => Number(v), z.number().int().min(1).max(100)).default(5),
  CB_OPEN_MS: z.preprocess((v) => Number(v), z.number().int().min(1000).max(300000)).default(10000),
  REQUIRE_WSS: z.string().optional(),
  ALLOWED_ORIGINS: z.string().optional(),
  SUBPROTOCOL: z
    .string()
    .regex(/^[A-Za-z0-9._~-]{1,64}$/)
    .optional(),
  HOST_SECRET: z.string().min(32).max(4096).optional(),
  HOST_SECRET_PREVIOUS: z.string().min(32).max(4096).optional(),
  ENABLE_AUTH: z.string().optional(),
  ENABLE_SESSION_AUTH: z.string().optional(),
  PAIRING_TOKEN_SECRET: z.string().min(32).max(4096).optional(),
  PAIRING_TOKEN_TTL_SECONDS: z
    .preprocess((v) => Number(v), z.number().int().min(30).max(600))
    .default(120),
  METRICS_SECRET: z.string().min(32).max(4096).optional(),
  HOST_CREDENTIALS_JSON: z
    .string()
    .max(1024 * 1024)
    .optional(),
  JWT_ISSUER: z.string().optional(),
  JWT_AUDIENCE: z.string().optional(),
  JWT_ALG: z.enum(['HS256', 'RS256', 'ES256', 'EdDSA']).optional(),
  JWT_SECRET: z.string().min(32).max(4096).optional(),
  JWKS_URL: z.string().optional(),
  JWKS_CACHE_TTL: z.preprocess((v) => Number(v), z.number().int().positive()).optional(),
  ROOMS_CLAIM: z.string().optional(),
  METERED_DOMAIN: z
    .string()
    .regex(/^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?$/)
    .optional(),
  METERED_API_KEY: z.string().min(16).max(4096).optional(),
  TURN_EXPIRY_SECONDS: z
    .preprocess((v) => Number(v), z.number().int().min(300).max(3600))
    .default(1200),
});

let parsed;
try {
  parsed = schema.parse(process.env);
} catch (e) {
  // Minimal console since logger might not be ready
  console.error('[config] Invalid configuration:', e.errors || e.message || String(e));
  process.exit(1);
}

function parseHostCredentials(raw) {
  if (!raw) return new Map();
  let value;
  try {
    value = JSON.parse(raw);
  } catch (error) {
    throw new Error('HOST_CREDENTIALS_JSON must be a JSON object', { cause: error });
  }
  if (!value || Array.isArray(value) || typeof value !== 'object') {
    throw new Error('HOST_CREDENTIALS_JSON must be a JSON object');
  }
  const entries = Object.entries(value);
  for (const [hostId, secret] of entries) {
    if (
      !UUID_PATTERN.test(hostId) ||
      typeof secret !== 'string' ||
      secret.length < 32 ||
      secret.length > 4096 ||
      !/^[A-Za-z0-9._~+/=-]+$/.test(secret)
    ) {
      throw new Error(
        'HOST_CREDENTIALS_JSON entries require UUID host IDs and secrets of at least 32 characters',
      );
    }
  }
  return new Map(entries.map(([hostId, secret]) => [hostId.toLowerCase(), secret]));
}

const pairingTokenSecret = parsed.PAIRING_TOKEN_SECRET;

const config = {
  env: parsed.NODE_ENV,
  wsPort: parsed.WS_PORT,
  mmPort: parsed.MATCHMAKER_PORT,
  bindHost: parsed.BIND_HOST,
  trustedProxyIps: new Set(
    (parsed.TRUSTED_PROXY_IPS || '')
      .split(',')
      .map((s) => s.trim())
      .filter(Boolean),
  ),
  redisUrl: parsed.REDIS_URL,
  redisPrefix: parsed.REDIS_KEY_PREFIX,
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
  allowedOrigins: (parsed.ALLOWED_ORIGINS || '')
    .split(',')
    .map((s) => s.trim())
    .filter(Boolean),
  subprotocol: parsed.SUBPROTOCOL || 'cloud-gaming-v1',
  hostSecret: parsed.HOST_SECRET,
  hostSecretPrevious: parsed.HOST_SECRET_PREVIOUS,
  hostCredentials: parseHostCredentials(parsed.HOST_CREDENTIALS_JSON),
  enableAuth: parsed.ENABLE_AUTH === 'true',
  enableSessionAuth: parsed.ENABLE_SESSION_AUTH !== 'false',
  pairingTokenSecret,
  pairingTokenTtlSeconds: parsed.PAIRING_TOKEN_TTL_SECONDS,
  metricsSecret: parsed.METRICS_SECRET,
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
    domain: parsed.METERED_DOMAIN || null,
    apiKey: parsed.METERED_API_KEY || null,
    expirySeconds: parsed.TURN_EXPIRY_SECONDS,
  },
};

if (config.env === 'production') {
  const missing = [];
  const redisUrl = new URL(config.redisUrl);
  if (!config.requireWss) missing.push('REQUIRE_WSS=true');
  if (config.hostCredentials.size === 0)
    missing.push('HOST_CREDENTIALS_JSON (per-host credentials)');
  if (!config.pairingTokenSecret || config.pairingTokenSecret.length < 32)
    missing.push('PAIRING_TOKEN_SECRET (at least 32 characters)');
  if (config.allowedOrigins.length === 0) missing.push('ALLOWED_ORIGINS');
  if (
    config.allowedOrigins.some(
      (origin) => !canonicalOrigin(origin) || !origin.startsWith('https://'),
    )
  )
    missing.push('ALLOWED_ORIGINS containing only canonical https:// origins');
  if (!config.metricsSecret || config.metricsSecret.length < 32)
    missing.push('METRICS_SECRET (at least 32 characters)');
  if (config.trustedProxyIps.size === 0)
    missing.push('TRUSTED_PROXY_IPS (exact reverse-proxy addresses)');
  if ([...config.trustedProxyIps].some((address) => isIP(address) === 0))
    missing.push('TRUSTED_PROXY_IPS containing only exact IP addresses');
  if (redisUrl.protocol !== 'rediss:') missing.push('REDIS_URL using rediss://');
  if (!redisUrl.username || !redisUrl.password) missing.push('REDIS_URL credentials');
  if (redisUrl.search || redisUrl.hash)
    missing.push('REDIS_URL without query strings or fragments');
  if (!config.metered.domain || !config.metered.apiKey)
    missing.push('METERED_DOMAIN and METERED_API_KEY for production TURN service');
  if (
    config.enableAuth &&
    (!config.jwt.issuer || !config.jwt.audience || (!config.jwt.jwksUrl && !config.jwt.secret))
  ) {
    missing.push('complete JWT issuer, audience, and JWKS/secret configuration');
  }
  if (config.enableAuth && config.jwt.jwksUrl) {
    const jwks = new URL(config.jwt.jwksUrl);
    if (jwks.protocol !== 'https:' || jwks.username || jwks.password || jwks.hash)
      missing.push('JWKS_URL using credential-free HTTPS');
  }
  if (
    config.enableAuth &&
    !config.jwt.jwksUrl &&
    (config.jwt.alg !== 'HS256' || !config.jwt.secret || config.jwt.secret.length < 32)
  ) {
    missing.push('HS256 JWT_SECRET of at least 32 characters, or HTTPS JWKS');
  }
  if (missing.length)
    throw new Error(`Production security configuration missing: ${missing.join(', ')}`);
}

if (
  config.enableSessionAuth &&
  (!config.pairingTokenSecret || config.pairingTokenSecret.length < 32)
) {
  throw new Error('Session authentication requires a distinct PAIRING_TOKEN_SECRET');
}

module.exports = { config };
