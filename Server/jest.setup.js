for (const key of [
  'HOST_SECRET',
  'HOST_SECRET_PREVIOUS',
  'HOST_CREDENTIALS_JSON',
  'JWT_SECRET',
  'JWKS_URL',
  'METERED_DOMAIN',
  'METERED_API_KEY',
  'ENABLE_AUTH',
  'ENABLE_SESSION_AUTH',
]) {
  delete process.env[key];
}
process.env.NODE_ENV = 'test';
process.env.BIND_HOST = '127.0.0.1';
process.env.PAIRING_TOKEN_SECRET =
  process.env.PAIRING_TOKEN_SECRET || 'test-pairing-secret-32-bytes-minimum';
process.env.METRICS_SECRET = process.env.METRICS_SECRET || 'test-metrics-secret-32-bytes-minimum';
process.env.ALLOWED_ORIGINS = process.env.ALLOWED_ORIGINS || 'http://localhost';
process.env.SUBPROTOCOL = process.env.SUBPROTOCOL || 'cloud-gaming-v1';
