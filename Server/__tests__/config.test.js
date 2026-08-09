describe('config module', () => {
  beforeEach(() => {
    jest.resetModules();
    for (const k of Object.keys(process.env)) {
      if (
        k.startsWith('WS_') ||
        k.startsWith('REDIS_') ||
        k.startsWith('ROOM_') ||
        k.startsWith('MESSAGE_') ||
        k.startsWith('BACKPRESSURE_') ||
        k.startsWith('HEARTBEAT_') ||
        k.startsWith('RATE_LIMIT_') ||
        k.startsWith('HEALTH_') ||
        k.startsWith('LOG_') ||
        k.startsWith('DRAIN_') ||
        k.startsWith('SHUTDOWN_') ||
        k.startsWith('CB_') ||
        [
          'NODE_ENV',
          'BIND_HOST',
          'TRUSTED_PROXY_IPS',
          'REQUIRE_WSS',
          'ALLOWED_ORIGINS',
          'SUBPROTOCOL',
          'HOST_SECRET',
          'HOST_SECRET_PREVIOUS',
          'HOST_CREDENTIALS_JSON',
          'PAIRING_TOKEN_SECRET',
          'METRICS_SECRET',
          'ENABLE_AUTH',
          'ENABLE_SESSION_AUTH',
          'JWT_ISSUER',
          'JWT_AUDIENCE',
          'JWT_ALG',
          'JWT_SECRET',
          'JWKS_URL',
          'JWKS_CACHE_TTL',
          'ROOMS_CLAIM',
          'METERED_DOMAIN',
          'METERED_API_KEY',
        ].includes(k)
      ) {
        delete process.env[k];
      }
    }
    process.env.NODE_ENV = 'test';
    process.env.BIND_HOST = '127.0.0.1';
    process.env.PAIRING_TOKEN_SECRET = '0123456789abcdef0123456789abcdef';
    process.env.ALLOWED_ORIGINS = 'http://127.0.0.1:8000';
    process.env.SUBPROTOCOL = 'cloud-gaming-v1';
  });

  it('loads defaults without throwing', () => {
    const { config } = require('../config');
    expect(config.wsPort).toBeGreaterThan(0);
    expect(config.redisUrl).toEqual(expect.stringContaining('redis://'));
    expect(config.healthPort).toBeGreaterThan(0);
    expect(config.prettyLogs).toBe(false);
  });

  it('applies environment overrides', () => {
    process.env.WS_PORT = '4000';
    process.env.REDIS_URL = 'redis://localhost:6380';
    process.env.ROOM_CAPACITY = '3';
    process.env.PRETTY_LOGS = 'true';
    jest.resetModules();
    const { config } = require('../config');
    expect(config.wsPort).toBe(4000);
    expect(config.redisUrl).toBe('redis://localhost:6380');
    expect(config.roomCapacity).toBe(3);
    expect(config.prettyLogs).toBe(true);
  });

  it('coerces booleans from strings', () => {
    process.env.REQUIRE_WSS = 'true';
    process.env.ENABLE_AUTH = 'false';
    jest.resetModules();
    const { config } = require('../config');
    expect(config.requireWss).toBe(true);
    expect(config.enableAuth).toBe(false);
  });

  it('splits allowed origins list', () => {
    process.env.ALLOWED_ORIGINS = 'https://a.com, https://b.com, ,  https://c.com ';
    jest.resetModules();
    const { config } = require('../config');
    expect(config.allowedOrigins).toEqual(['https://a.com', 'https://b.com', 'https://c.com']);
  });

  it('fails fast on invalid numeric values', () => {
    process.env.WS_PORT = 'NaN';
    jest.resetModules();
    const exitSpy = jest.spyOn(process, 'exit').mockImplementation(() => {
      throw new Error('exit');
    });
    const errorSpy = jest.spyOn(console, 'error').mockImplementation(() => {});
    expect(() => require('../config')).toThrow('exit');
    expect(errorSpy).toHaveBeenCalled();
    exitSpy.mockRestore();
    errorSpy.mockRestore();
  });

  it('parses bounded proxy and per-host credential settings', () => {
    const hostId = '123e4567-e89b-12d3-a456-426614174000';
    process.env.TRUSTED_PROXY_IPS = '127.0.0.1, ::1';
    process.env.HOST_CREDENTIALS_JSON = JSON.stringify({ [hostId]: 'h'.repeat(32) });
    jest.resetModules();
    const { config } = require('../config');
    expect(config.trustedProxyIps.has('::1')).toBe(true);
    expect(config.hostCredentials.get(hostId)).toBe('h'.repeat(32));
  });

  it('rejects malformed host credential maps', () => {
    process.env.HOST_CREDENTIALS_JSON = JSON.stringify({ 'not-a-uuid': 'short' });
    jest.resetModules();
    expect(() => require('../config')).toThrow('HOST_CREDENTIALS_JSON entries');
  });

  it('rejects session auth without a strong pairing secret', () => {
    delete process.env.PAIRING_TOKEN_SECRET;
    process.env.ENABLE_SESSION_AUTH = 'true';
    jest.resetModules();
    expect(() => require('../config')).toThrow('Session authentication requires');
  });

  it('fails closed on incomplete production security configuration', () => {
    process.env.NODE_ENV = 'production';
    jest.resetModules();
    expect(() => require('../config')).toThrow('Production security configuration missing');
  });

  it('accepts a complete production security configuration', () => {
    const hostId = '123e4567-e89b-12d3-a456-426614174000';
    process.env.NODE_ENV = 'production';
    process.env.REQUIRE_WSS = 'true';
    process.env.REDIS_URL = 'rediss://service-user:service-password@redis.example:6380/0';
    process.env.HOST_CREDENTIALS_JSON = JSON.stringify({ [hostId]: 'h'.repeat(32) });
    process.env.PAIRING_TOKEN_SECRET = 'p'.repeat(32);
    process.env.METRICS_SECRET = 'm'.repeat(32);
    process.env.ALLOWED_ORIGINS = 'https://play.example';
    process.env.TRUSTED_PROXY_IPS = '10.0.0.10';
    process.env.METERED_DOMAIN = 'example-project';
    process.env.METERED_API_KEY = 't'.repeat(32);
    jest.resetModules();
    const { config } = require('../config');
    expect(config.env).toBe('production');
    expect(config.requireWss).toBe(true);
    expect(config.redisUrl).toMatch(/^rediss:/);
  });
});
