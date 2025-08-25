describe('config module', () => {
	beforeEach(() => {
		jest.resetModules();
		for (const k of Object.keys(process.env)) {
			if (k.startsWith('WS_') || k.startsWith('REDIS_') || k.startsWith('ROOM_') || k.startsWith('MESSAGE_') || k.startsWith('BACKPRESSURE_') || k.startsWith('HEARTBEAT_') || k.startsWith('RATE_LIMIT_') || k.startsWith('HEALTH_') || k.startsWith('LOG_') || k.startsWith('DRAIN_') || k.startsWith('SHUTDOWN_') || k.startsWith('CB_') || ['NODE_ENV','REQUIRE_WSS','ALLOWED_ORIGINS','SUBPROTOCOL','ENABLE_AUTH','JWT_ISSUER','JWT_AUDIENCE','JWT_ALG','JWT_SECRET','JWKS_URL','JWKS_CACHE_TTL','ROOMS_CLAIM'].includes(k)) {
				delete process.env[k];
			}
		}
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
		const exitSpy = jest.spyOn(process, 'exit').mockImplementation(() => { throw new Error('exit'); });
		const errorSpy = jest.spyOn(console, 'error').mockImplementation(() => {});
		expect(() => require('../config')).toThrow('exit');
		expect(errorSpy).toHaveBeenCalled();
		exitSpy.mockRestore();
		errorSpy.mockRestore();
	});
});


