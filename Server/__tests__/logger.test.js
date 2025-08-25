describe('logger module', () => {
	beforeEach(() => {
		jest.resetModules();
	});

	it('creates a logger and logs without throwing', () => {
		const { logger } = require('../logger');
		expect(() => logger.info({ test: true }, 'hello')).not.toThrow();
	});

	it('redacts sensitive fields', () => {
		process.env.PRETTY_LOGS = 'false';
		jest.resetModules();
		const { logger } = require('../logger');
		const spy = jest.spyOn(process.stdout, 'write').mockImplementation(() => true);
		logger.info({ authorization: 'Bearer abc', token: '123' }, 'test');
		const output = spy.mock.calls.map(c => String(c[0])).join('');
		spy.mockRestore();
		expect(output).toContain('[Redacted]');
		expect(output).not.toContain('Bearer abc');
		expect(output).not.toContain('123');
	});

	it('honors log level', () => {
		process.env.LOG_LEVEL = 'error';
		jest.resetModules();
		const { logger } = require('../logger');
		const spy = jest.spyOn(process.stdout, 'write').mockImplementation(() => true);
		logger.info({ a: 1 }, 'should not appear');
		logger.error({ b: 2 }, 'should appear');
		const output = spy.mock.calls.map(c => String(c[0])).join('');
		spy.mockRestore();
		expect(output).toContain('should appear');
		expect(output).not.toContain('should not appear');
	});
});


