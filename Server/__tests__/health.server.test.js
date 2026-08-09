const http = require('http');
const { startHealthServer } = require('../health');
const { stopDefaultMetrics } = require('../metrics');

jest.setTimeout(20000);

function get(pathname, port) {
  return new Promise((resolve, reject) => {
    http
      .get(
        {
          hostname: '127.0.0.1',
          port,
          path: pathname,
          headers: { Authorization: `Bearer ${process.env.METRICS_SECRET}` },
        },
        resolve,
      )
      .on('error', reject);
  });
}

function waitForListening(server) {
  if (server.listening) return Promise.resolve();
  return new Promise((resolve, reject) => {
    server.once('listening', resolve);
    server.once('error', reject);
  });
}

describe('health server endpoints', () => {
  afterAll(() => {
    try {
      stopDefaultMetrics();
    } catch (_) {}
  });
  it('responds to /healthz with 200', async () => {
    const srv = startHealthServer({});
    try {
      await waitForListening(srv);
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
      await waitForListening(srv);
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
      await waitForListening(srv);
      // Touch some metrics
      const { setActiveConnections, setLocalRooms, incMessagesForwarded } = require('../metrics');
      setActiveConnections(3);
      setLocalRooms(1);
      incMessagesForwarded();
      const res = await get('/metrics', srv.address().port);
      expect(res.statusCode).toBe(200);
      const unauthorized = await new Promise((resolve, reject) => {
        http
          .get({ hostname: '127.0.0.1', port: srv.address().port, path: '/metrics' }, resolve)
          .on('error', reject);
      });
      expect(unauthorized.statusCode).toBe(404);
      const notFound = await get('/nope', srv.address().port);
      expect(notFound.statusCode).toBe(404);
    } finally {
      await new Promise((r) => srv.close(() => r()));
    }
  });
});
