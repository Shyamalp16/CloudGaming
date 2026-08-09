const http = require('http');
const { config } = require('./config');
const { logger } = require('./logger');
const { metricsHandler } = require('./metrics');
const { bearerToken, secureEqual } = require('./security');

function startHealthServer({ readinessCheck }) {
  const log = logger.child({ svc: 'health' });
  const server = http.createServer(async (req, res) => {
    res.setHeader('Cache-Control', 'no-store');
    res.setHeader('X-Content-Type-Options', 'nosniff');
    if (req.url === '/healthz') {
      res.writeHead(200, { 'Content-Type': 'text/plain' });
      res.end('ok');
      return;
    }
    if (req.url === '/readyz') {
      try {
        const ready = readinessCheck ? await readinessCheck() : true;
        if (ready) {
          res.writeHead(200, { 'Content-Type': 'text/plain' });
          res.end('ready');
        } else {
          res.writeHead(503, { 'Content-Type': 'text/plain' });
          res.end('not-ready');
        }
      } catch (e) {
        res.writeHead(503, { 'Content-Type': 'text/plain' });
        res.end('not-ready');
      }
      return;
    }
    if (req.url === '/metrics') {
      if (!config.metricsSecret || !secureEqual(bearerToken(req.headers), config.metricsSecret)) {
        res.writeHead(404, { 'Content-Type': 'text/plain' });
        res.end('not-found');
        return;
      }
      return metricsHandler(req, res);
    }
    res.writeHead(404, { 'Content-Type': 'text/plain' });
    res.end('not-found');
  });

  server.requestTimeout = 10000;
  server.headersTimeout = 5000;
  server.maxHeadersCount = 32;
  server.listen(config.healthPort, config.bindHost, () => {
    log.info({ port: config.healthPort }, 'Health server listening');
  });

  return server;
}

module.exports = { startHealthServer };
