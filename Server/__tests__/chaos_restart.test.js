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
	const payload = { sub: 'chaos-restart', iss, aud, rooms: [roomId] };
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

describe('Chaos: server restart and client reconnection', () => {
	it('clients reconnect to new server and resume signaling', async () => {
		const wsPort1 = await getFreePort();
		const healthPort1 = await getFreePort();
		const s1 = startServer({ wsPort: wsPort1, healthPort: healthPort1 });
		try {
			await waitForReady(healthPort1, 20000);
			const roomId = `chaos-restart-${Date.now()}`;
			const token = makeAuthToken(roomId);
			const qs = token ? `&token=${token}` : '';
			const base1 = `ws://127.0.0.1:${wsPort1}`;
			let c1 = await connectClient(`${base1}/?roomId=${roomId}${qs}`);
			let c2 = await connectClient(`${base1}/?roomId=${roomId}${qs}`);

			await new Promise((r) => setTimeout(r, 100));
			c1.send(JSON.stringify({ type: 'control', action: 'before-restart' }));
			await new Promise((resolve, reject) => { c2.once('message', () => resolve()); c2.once('error', reject); });

			// Restart: kill s1 and bring up s2 on a new port
			try { s1.kill('SIGTERM'); } catch (_) {}
			try { await waitForExit(s1, 8000); } catch (_) {}
			try { c1.close(); } catch (_) {}
			try { c2.close(); } catch (_) {}

			const wsPort2 = await getFreePort();
			const healthPort2 = await getFreePort();
			const s2 = startServer({ wsPort: wsPort2, healthPort: healthPort2 });
			try {
				await waitForReady(healthPort2, 20000);
				const base2 = `ws://127.0.0.1:${wsPort2}`;
				c1 = await connectClient(`${base2}/?roomId=${roomId}${qs}`);
				c2 = await connectClient(`${base2}/?roomId=${roomId}${qs}`);
				await new Promise((r) => setTimeout(r, 100));
				c1.send(JSON.stringify({ type: 'control', action: 'after-restart' }));
				const msg = await new Promise((resolve, reject) => {
					c2.once('message', (m) => { try { resolve(JSON.parse(m.toString())); } catch (e) { reject(e); } });
					c2.once('error', reject);
				});
				expect(msg).toEqual({ type: 'control', action: 'after-restart' });
			} finally {
				try { s2.kill('SIGTERM'); } catch (_) {}
				try { await waitForExit(s2, 8000); } catch (_) {}
			}
		} finally {
			// s1 is already killed
		}
	});
});


