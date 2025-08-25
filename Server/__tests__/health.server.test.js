const http = require('http');
const { startHealthServer } = require('../health');
const { register, stopDefaultMetrics } = require('../metrics');

jest.setTimeout(20000);

function get(pathname, port) {
	return new Promise((resolve, reject) => {
		http.get({ hostname: '127.0.0.1', port, path: pathname }, resolve).on('error', reject);
	});
}

describe('health server endpoints', () => {
    afterAll(() => { try { stopDefaultMetrics(); } catch (_) {} });
	it('responds to /healthz with 200', async () => {
		const srv = startHealthServer({});
		try {
			const res = await get('/healthz', srv.address().port);
			expect(res.statusCode).toBe(200);
		} finally {
			await new Promise((r) => srv.close(() => r()));
		}
	});

	it('responds to /readyz with 200 and 503 based on readinessCheck', async () => {
		let ready = false;
		const srv = startHealthServer({ readinessCheck: async () => ready });
		try {
			let res = await get('/readyz', srv.address().port);
			expect([200, 503]).toContain(res.statusCode);
			ready = true;
			res = await get('/readyz', srv.address().port);
			expect(res.statusCode).toBe(200);
			ready = false;
			res = await get('/readyz', srv.address().port);
			expect(res.statusCode).toBe(503);
		} finally {
			await new Promise((r) => srv.close(() => r()));
		}
	});

	it('exposes /metrics and 404 for unknown paths', async () => {
		const srv = startHealthServer({});
		try {
			// Touch some metrics
			const { setActiveConnections, setLocalRooms, incMessagesForwarded } = require('../metrics');
			setActiveConnections(3);
			setLocalRooms(1);
			incMessagesForwarded();
			const res = await get('/metrics', srv.address().port);
			expect(res.statusCode).toBe(200);
			const notFound = await get('/nope', srv.address().port);
			expect(notFound.statusCode).toBe(404);
		} finally {
			await new Promise((r) => srv.close(() => r()));
		}
	});
});


