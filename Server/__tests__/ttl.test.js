const { spawn } = require('child_process');
const path = require('path');
const net = require('net');
const WebSocket = require('ws');
const jwt = require('jsonwebtoken');
const { createClient } = require('redis');

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
	const payload = { sub: 'ttl', iss, aud, rooms: [roomId] };
	return jwt.sign(payload, secret, { algorithm: 'HS256', expiresIn: '5m' });
}

function connectClient(url, timeoutMs = 10000) {
	return new Promise((resolve, reject) => {
		const protocols = process.env.SUBPROTOCOL ? [process.env.SUBPROTOCOL] : undefined;
		const ws = new WebSocket(url, protocols, { headers: { origin: 'http://localhost' } });
		const t = setTimeout(() => { try { ws.terminate(); } catch (_) {} reject(new Error('WS connect timeout')); }, timeoutMs);
		ws.once('open', () => { clearTimeout(t); resolve(ws); });
		ws.once('error', (e) => { clearTimeout(t); reject(e); });
	});
}

describe('Room TTL logic', () => {
	it('sets TTL on empty rooms and expires key', async () => {
		const wsPort = await getFreePort();
		const healthPort = await getFreePort();
		// Use a very small room TTL for quick expiry
		const child = startServer({ wsPort, healthPort, extraEnv: { ROOM_TTL_SECONDS: '2' } });
		const redisUrl = process.env.REDIS_URL || 'redis://127.0.0.1:6379';
		const redis = createClient({ url: redisUrl });
		await redis.connect();
		const roomId = `ttl-room-${Date.now()}`;
		const roomKey = `room:${roomId}`;
		try {
			await Promise.race([
				waitForReady(healthPort, 20000),
				waitForExit(child, 20000).then(({ code, signal }) => { throw new Error(`Server exited early: ${code}:${signal}`); }),
			]);
			const base = `ws://127.0.0.1:${wsPort}`;
			const token = makeAuthToken(roomId);
			const qs = token ? `&token=${token}` : '';
			const c1 = await connectClient(`${base}/?roomId=${roomId}${qs}`);
			// small settle so join is processed
			await new Promise((r) => setTimeout(r, 150));
			// Close client to trigger leave; room becomes empty -> EXPIRE set on key
			await new Promise((r) => { try { c1.close(); } catch (_) {} setTimeout(r, 50); });
			// Wait a moment for server to process leave and set TTL
			await new Promise((r) => setTimeout(r, 150));
			const ttl = await redis.ttl(roomKey);
			// ttl should be >= 1 (seconds) if key exists; -2 means key missing already
			expect(ttl === -2 || ttl >= 1).toBe(true);
			// After ~3s key should be gone
			await new Promise((r) => setTimeout(r, 2500));
			const exists = await redis.exists(roomKey);
			expect(exists).toBe(0);
		} finally {
			try { child.kill('SIGTERM'); } catch (_) {}
			try { await waitForExit(child, 5000); } catch (_) {}
			try { await redis.quit(); } catch (_) {}
		}
	});
});


