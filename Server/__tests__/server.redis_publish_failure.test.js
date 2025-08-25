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

jest.setTimeout(90000);

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

function startServer({ wsPort, healthPort, extraEnv = {} }) {
	const serverPath = path.join(__dirname, '..', 'ScalableSignalingServer.js');
	const child = spawn(process.execPath, [serverPath], {
		env: {
			...process.env,
			WS_PORT: String(wsPort),
			HEALTH_PORT: String(healthPort),
			REDIS_URL: process.env.REDIS_URL || 'redis://127.0.0.1:6379',
			PRETTY_LOGS: 'false',
			...extraEnv,
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
	const payload = { sub: 'redis-fail', iss, aud, rooms: [roomId] };
	return jwt.sign(payload, secret, { algorithm: 'HS256', expiresIn: '5m' });
}

function connect(url, timeoutMs = 15000) {
	return new Promise((resolve, reject) => {
		const protocols = process.env.SUBPROTOCOL ? [process.env.SUBPROTOCOL] : undefined;
		const ws = new WebSocket(url, protocols, { headers: { origin: 'http://localhost' } });
		const t = setTimeout(() => { try { ws.terminate(); } catch (_) {} reject(new Error('WS connect timeout')); }, timeoutMs);
		ws.once('open', () => { clearTimeout(t); resolve(ws); });
		ws.once('error', (e) => { clearTimeout(t); reject(e); });
	});
}

describe('Redis publish failure path', () => {
	it('continues operation when publish fails (coverage of catch)', async () => {
		const wsPort = await getFreePort();
		const healthPort = await getFreePort();
		const child = startServer({ wsPort, healthPort });
		try {
			await waitForReady(healthPort, 25000);
			const roomId = `redis-fail-${Date.now()}`;
			const base = `ws://127.0.0.1:${wsPort}`;
			const token = makeAuthToken(roomId);
			const qs = token ? `&token=${token}` : '';
			const c1 = await connect(`${base}/?roomId=${roomId}${qs}`);
			const c2 = await connect(`${base}/?roomId=${roomId}${qs}`);

			await new Promise((r) => setTimeout(r, 150));

			// Pause docker redis best-effort to trigger publish failure
			try { await new Promise((resolve) => spawn('docker', ['pause', 'redis']).on('exit', () => resolve())); } catch (_) {}

			// Send a message; local fanout should still deliver even if publish fails
			const received = new Promise((resolve, reject) => {
				c2.once('message', (m) => { try { resolve(JSON.parse(m.toString())); } catch (e) { reject(e); } });
				c2.once('error', reject);
			});
			c1.send(JSON.stringify({ type: 'control', action: 'publish-fail' }));
			const data = await Promise.race([ received, new Promise((_, rej) => setTimeout(() => rej(new Error('no local fanout')), 15000)) ]);
			expect(data).toEqual({ type: 'control', action: 'publish-fail' });

			try { await new Promise((resolve) => spawn('docker', ['unpause', 'redis']).on('exit', () => resolve())); } catch (_) {}

			try { c1.close(); } catch (_) {}
			try { c2.close(); } catch (_) {}
		} finally {
			try { child.kill('SIGTERM'); } catch (_) {}
			try { await waitForExit(child, 5000); } catch (_) {}
		}
	});
});


