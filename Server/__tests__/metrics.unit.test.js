describe('metrics module', () => {
	afterAll(() => { try { require('../metrics').stopDefaultMetrics(); } catch (_) {} });

	it('increments and sets metrics without throwing', () => {
		const m = require('../metrics');
		expect(() => m.setActiveConnections(5)).not.toThrow();
		expect(() => m.setLocalRooms(2)).not.toThrow();
		expect(() => m.incMessagesForwarded()).not.toThrow();
		expect(() => m.incSchemaRejects()).not.toThrow();
		expect(() => m.incRateLimitDrops()).not.toThrow();
		expect(() => m.incBackpressureCloses()).not.toThrow();
		const stopRedis = m.startRedisTimer('publish');
		const stopFan = m.startFanoutTimer();
		stopRedis();
		stopFan();
	});
});


