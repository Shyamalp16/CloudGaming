const { spawn } = require('child_process');
const path = require('path');
const net = require('net');
const WebSocket = require('ws');
const jwt = require('jsonwebtoken');

function getFreePort() {
	return new Promise((resolve, reject) => {
		const srv = net.createServer();
		srv.listen(0, '127.0.0.1', () => {
			const addr = srv.address();
			const port = typeof addr === 'object' && addr ? addr.port : undefined;
			srv.close((err) => (err ? reject(err) : resolve(port)));
		});
		srv.on('error', reject);
	});
}

jest.setTimeout(60000);

function waitForExit(child, timeoutMs = 15000) {
	return new Promise((resolve, reject) => {
		const timer = setTimeout(() => reject(new Error('Child did not exit in time')), timeoutMs);
		child.once('exit', (code, signal) => { clearTimeout(timer); resolve({ code, signal }); });
	});
}

async function waitForReady(healthPort, timeoutMs = 20000) {
	const deadline = Date.now() + timeoutMs;
	while (Date.now() < deadline) {
		try {
			const res = await fetch(`http://127.0.0.1:${healthPort}/readyz`);
			if (res.status === 200) return true;
		} catch (_) {}
		await new Promise((r) => setTimeout(r, 250));
	}
	throw new Error('Server not ready in time');
}

function startServer({ wsPort, healthPort }) {
	const serverPath = path.join(__dirname, '..', 'ScalableSignalingServer.js');
	const child = spawn(process.execPath, [serverPath], {
		env: {
			...process.env,
			WS_PORT: String(wsPort),
			HEALTH_PORT: String(healthPort),
			REDIS_URL: process.env.REDIS_URL || 'redis://127.0.0.1:6379',
			PRETTY_LOGS: 'false',
			ENABLE_AUTH: 'true',
			JWT_ALG: 'HS256',
			JWT_SECRET: 'test-secret',
			JWT_ISSUER: 'http://localhost',
			JWT_AUDIENCE: 'test',
		},
		cwd: path.join(__dirname, '..'),
		stdio: ['ignore', 'pipe', 'pipe'],
	});
	return child;
}

function connect(url, subprotocol) {
	return new Promise((resolve, reject) => {
		const protocols = subprotocol ? [subprotocol] : undefined;
		const ws = new WebSocket(url, protocols, { headers: { origin: 'http://localhost' } });
		ws.once('open', () => resolve(ws));
		ws.once('error', reject);
	});
}

describe('JWT auth and room authorization', () => {
	it('accepts authorized token and rejects unauthorized room', async () => {
		const wsPort = await getFreePort();
		const healthPort = await getFreePort();
		const child = startServer({ wsPort, healthPort });
		try {
			await Promise.race([
				waitForReady(healthPort, 20000),
				waitForExit(child, 20000).then(({ code, signal }) => { throw new Error(`Server exited early: ${code}:${signal}`); }),
			]);

			const allowedRoom = 'room-allowed';
			const forbiddenRoom = 'room-forbidden';
			const token = jwt.sign({ sub: 'u1', iss: 'http://localhost', aud: 'test', rooms: [allowedRoom] }, 'test-secret', { algorithm: 'HS256', expiresIn: '5m' });

			// Allowed room
			const okUrl = `ws://127.0.0.1:${wsPort}/?roomId=${allowedRoom}&token=${token}`;
			const wsOk = await connect(okUrl, process.env.SUBPROTOCOL);
			expect(wsOk.readyState).toBe(WebSocket.OPEN);
			wsOk.close();

			// Forbidden room: should close quickly with 1008
			const badUrl = `ws://127.0.0.1:${wsPort}/?roomId=${forbiddenRoom}&token=${token}`;
			const wsBad = await connect(badUrl, process.env.SUBPROTOCOL);
			const closeCode = await new Promise((resolve, reject) => {
				const t = setTimeout(() => reject(new Error('forbidden ws not closed')), 5000);
				wsBad.once('close', (code) => { clearTimeout(t); resolve(code); });
			});
			expect([1008, 1000]).toContain(closeCode);
		} finally {
			try { child.kill('SIGTERM'); } catch (_) {}
			try { await waitForExit(child, 5000); } catch (_) {}
		}
	});
});


