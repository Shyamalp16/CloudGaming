it('exposes metrics via /metrics', async () => {
	const http = require('http');
	const { startHealthServer } = require('../health');
	const { config } = require('../config');

	// Start a one-off health server
	const server = startHealthServer({});

	// Fetch metrics
	const res = await new Promise((resolve, reject) => {
		http.get({ hostname: '127.0.0.1', port: config.healthPort, path: '/metrics' }, resolve).on('error', reject);
	});

	expect(res.statusCode).toBe(200);

	await new Promise((r) => server.close(() => r()));
});


