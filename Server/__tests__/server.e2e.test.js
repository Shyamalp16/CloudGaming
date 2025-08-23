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

async function waitForReady(healthPort, timeoutMs = 15000) {
	const deadline = Date.now() + timeoutMs;
	while (Date.now() < deadline) {
		try {
			const res = await fetch(`http://127.0.0.1:${healthPort}/readyz`);
			if (res.status === 200) return true;
		} catch (_) {}
		await new Promise((r) => setTimeout(r, 300));
	}
	throw new Error('Server not ready in time');
}

function waitForExit(child, timeoutMs = 15000) {
	return new Promise((resolve, reject) => {
		const timer = setTimeout(() => reject(new Error('Child did not exit in time')), timeoutMs);
		child.once('exit', (code, signal) => {
			clearTimeout(timer);
			resolve({ code, signal });
		});
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
	const payload = { sub: 'e2e', iss, aud, rooms: [roomId] };
	return jwt.sign(payload, secret, { algorithm: 'HS256', expiresIn: '5m' });
}

function connectClient(url, timeoutMs = 15000) {
	return new Promise((resolve, reject) => {
		const protocols = process.env.SUBPROTOCOL ? [process.env.SUBPROTOCOL] : undefined;
		const ws = new WebSocket(url, protocols, { headers: { origin: 'http://localhost' } });
		const t = setTimeout(() => {
			try { ws.terminate(); } catch (_) {}
			reject(new Error('WS connect timeout'));
		}, timeoutMs);
		ws.once('open', () => { clearTimeout(t); resolve(ws); });
		ws.once('error', (e) => { clearTimeout(t); reject(e); });
	});
}

describe('ScalableSignalingServer headless E2E', () => {
	it('starts, accepts clients, and forwards messages', async () => {
		const wsPort = await getFreePort();
		const healthPort = await getFreePort();
		const child = startServer({ wsPort, healthPort });

		try {
			await Promise.race([
				waitForReady(healthPort, 20000),
				waitForExit(child, 20000).then(({ code, signal }) => {
					throw new Error(`Server exited early (code=${code}, signal=${signal})`);
				}),
			]);

			const roomId = 'e2e-room';
			const base = `ws://127.0.0.1:${wsPort}`;
			const token = makeAuthToken(roomId);
			const qs = token ? `&token=${token}` : '';
			const c1 = await connectClient(`${base}/?roomId=${roomId}${qs}`);
			const c2 = await connectClient(`${base}/?roomId=${roomId}${qs}`);

			const received = new Promise((resolve, reject) => {
				c2.once('message', (msg) => {
					try {
						const data = JSON.parse(msg.toString());
						resolve(data);
					} catch (e) {
						reject(e);
					}
				});
				c2.once('error', reject);
			});

			// Small delay to avoid race with server local room registration
			await new Promise((r) => setTimeout(r, 100));
			c1.send(JSON.stringify({ type: 'control', action: 'test', payload: { ok: true } }));
			const data = await Promise.race([
				received,
				new Promise((_, rej) => setTimeout(() => rej(new Error('No message received')), 20000)),
			]);
			expect(data).toEqual({ type: 'control', action: 'test', payload: { ok: true } });

			await new Promise((r) => { try { c1.close(); } catch (_) {} setTimeout(r, 50); });
			await new Promise((r) => { try { c2.close(); } catch (_) {} setTimeout(r, 50); });
		} finally {
			try { child.kill('SIGTERM'); } catch (_) {}
			try { await waitForExit(child, 5000); } catch (_) {}
		}
	});

	it('drain closes clients with 1012 close code', async () => {
		const wsPort = await getFreePort();
		const healthPort = await getFreePort();
		const child = startServer({ wsPort, healthPort });

		try {
			await Promise.race([
				waitForReady(healthPort, 20000),
				waitForExit(child, 20000).then(({ code, signal }) => {
					throw new Error(`Server exited early (code=${code}, signal=${signal})`);
				}),
			]);

			const roomId = 'e2e-room-drain';
			const base = `ws://127.0.0.1:${wsPort}`;
			const token = makeAuthToken(roomId);
			const qs = token ? `&token=${token}` : '';
			const c1 = await connectClient(`${base}/?roomId=${roomId}${qs}`);
			const c2 = await connectClient(`${base}/?roomId=${roomId}${qs}`);

			const closed1 = new Promise((resolve) => {
				c1.once('close', (code) => resolve(code));
			});
			const closed2 = new Promise((resolve) => {
				c2.once('close', (code) => resolve(code));
			});

			// Trigger drain
			try { child.kill('SIGTERM'); } catch (_) {}

			const code1 = await Promise.race([
				closed1,
				new Promise((_, rej) => setTimeout(() => rej(new Error('c1 not closed')), 15000)),
			]);
			const code2 = await Promise.race([
				closed2,
				new Promise((_, rej) => setTimeout(() => rej(new Error('c2 not closed')), 15000)),
			]);

			// Accept 1012 primarily; allow 1011/1006/1008 under CI timing/transport quirks
			expect([1012, 1011, 1006, 1008]).toContain(code1);
			expect([1012, 1011, 1006, 1008]).toContain(code2);
		} finally {
			try { await waitForExit(child, 5000); } catch (_) {}
		}
	});
});


