const http = require('http');
const { config } = require('./config');
const { logger } = require('./logger');

function startHealthServer({ readinessCheck }) {
	const log = logger.child({ svc: 'health' });
	const server = http.createServer(async (req, res) => {
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
		res.writeHead(404, { 'Content-Type': 'text/plain' });
		res.end('not-found');
	});

	server.listen(config.healthPort, () => {
		log.info({ port: config.healthPort }, 'Health server listening');
	});

	return server;
}

module.exports = { startHealthServer };


