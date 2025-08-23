const http = require('http');

jest.setTimeout(30000);

it('metrics endpoint is exposed by health server (smoke)', async () => {
	const { config } = require('../config');
	const res = await new Promise((resolve, reject) => {
		http.get({ hostname: '127.0.0.1', port: config.healthPort, path: '/metrics' }, resolve).on('error', reject);
	});
	expect(res.statusCode).toBe(200);
});


