describe('config module', () => {
	it('loads defaults without throwing', () => {
		// Ensure requiring config works and returns required fields
		const { config } = require('../config');
		expect(config.wsPort).toBeGreaterThan(0);
		expect(config.redisUrl).toEqual(expect.stringContaining('redis://'));
		expect(config.healthPort).toBeGreaterThan(0);
	});
});


