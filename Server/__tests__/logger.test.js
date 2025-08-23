describe('logger module', () => {
	it('creates a logger and logs without throwing', () => {
		const { logger } = require('../logger');
		expect(() => logger.info({ test: true }, 'hello')).not.toThrow();
	});
});


