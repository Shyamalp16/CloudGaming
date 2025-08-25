const { spawn } = require('child_process');
const path = require('path');
const net = require('net');
const WebSocket = require('ws');
const jwt = require('jsonwebtoken');

// NOTE: This test assumes the CI provides control of Redis lifecycle or uses a local Docker engine.
// If Redis cannot be stopped in CI, this test behaves as a smoke test that still validates messaging.

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

jest.setTimeout(120000);

async function waitForReady(healthPort, timeoutMs = 25000) {
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

function waitForExit(child, timeoutMs = 15000) {
	return new Promise((resolve, reject) => {
		const timer = setTimeout(() => reject(new Error('Child did not exit in time')), timeoutMs);
		child.once('exit', (code, signal) => { clearTimeout(timer); resolve({ code, signal }); });
	});
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
		},
		cwd: path.join(__dirname, '..'),
		stdio: ['ignore', 'pipe', 'pipe'],
	});
	return child;
}

function makeAuthToken(roomId) {
	if (process.env.ENABLE_AUTH !== 'true') return '';
	const secret = process.env.JWT_SECRET || 'test-secret';
	const iss = process.env.JWT_ISSUER || 'http://localhost';
	const aud = process.env.JWT_AUDIENCE || 'test';
	const payload = { sub: 'chaos-redis', iss, aud, rooms: [roomId] };
	return jwt.sign(payload, secret, { algorithm: 'HS256', expiresIn: '5m' });
}

function connectClient(url, timeoutMs = 15000) {
	return new Promise((resolve, reject) => {
		const protocols = process.env.SUBPROTOCOL ? [process.env.SUBPROTOCOL] : undefined;
		const ws = new WebSocket(url, protocols, { headers: { origin: 'http://localhost' } });
		const t = setTimeout(() => { try { ws.terminate(); } catch (_) {} reject(new Error('WS connect timeout')); }, timeoutMs);
		ws.once('open', () => { clearTimeout(t); resolve(ws); });
		ws.once('error', (e) => { clearTimeout(t); reject(e); });
	});
}

describe('Chaos: Redis outage mid-session', () => {
	it('clients continue local messaging and recover after Redis returns', async () => {
		const wsPort = await getFreePort();
		const healthPort = await getFreePort();
		const child = startServer({ wsPort, healthPort });
		try {
			await waitForReady(healthPort, 25000);
			const roomId = `chaos-redis-${Date.now()}`;
			const token = makeAuthToken(roomId);
			const qs = token ? `&token=${token}` : '';
			const base = `ws://127.0.0.1:${wsPort}`;
			const c1 = await connectClient(`${base}/?roomId=${roomId}${qs}`);
			const c2 = await connectClient(`${base}/?roomId=${roomId}${qs}`);

			await new Promise((r) => setTimeout(r, 150));
			// Baseline: ensure messaging works
			await new Promise((resolve, reject) => {
				c2.once('message', () => resolve());
				c2.once('error', reject);
				c1.send(JSON.stringify({ type: 'control', action: 'baseline' }));
			});

			// Attempt to pause Redis via docker if available (best-effort, non-fatal if fails)
			await new Promise((r) => setTimeout(r, 100));
			try {
				await new Promise((resolve) => {
					const ctl = spawn('docker', ['pause', 'redis']);
					ctl.on('exit', () => resolve());
				});
			} catch (_) {}

			// During outage: local fanout should still work if both clients on same instance
			const during = new Promise((resolve, reject) => {
				c2.once('message', (m) => resolve(m));
				c2.once('error', reject);
				c1.send(JSON.stringify({ type: 'control', action: 'during-outage' }));
			});
			await Promise.race([ during, new Promise((_, rej) => setTimeout(() => rej(new Error('no message during outage')), 15000)) ]);

			// Resume Redis (best-effort)
			try {
				await new Promise((resolve) => {
					const ctl = spawn('docker', ['unpause', 'redis']);
					ctl.on('exit', () => resolve());
				});
			} catch (_) {}

			await new Promise((r) => setTimeout(r, 500));
			// After recovery: messaging should still work
			const after = new Promise((resolve, reject) => {
				c2.once('message', (m) => resolve(m));
				c2.once('error', reject);
				c1.send(JSON.stringify({ type: 'control', action: 'after-recovery' }));
			});
			await Promise.race([ after, new Promise((_, rej) => setTimeout(() => rej(new Error('no message after recovery')), 15000)) ]);

		} finally {
			try { child.kill('SIGTERM'); } catch (_) {}
			try { await waitForExit(child, 5000); } catch (_) {}
		}
	});
});


