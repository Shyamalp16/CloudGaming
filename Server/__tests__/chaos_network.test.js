const { spawn } = require('child_process');
const path = require('path');
const net = require('net');
const jwt = require('jsonwebtoken');

// A thin wrapper around ws to inject delay and drop probability on send/receive
class JitterWS {
	constructor(ws, { sendDelayMs = 0, recvDelayMs = 0, dropProb = 0 } = {}) {
		this.ws = ws;
		this.sendDelayMs = sendDelayMs;
		this.recvDelayMs = recvDelayMs;
		this.dropProb = dropProb;
	}

	send(data) {
		if (Math.random() < this.dropProb) return; // drop
		setTimeout(() => {
			try { this.ws.send(data); } catch (_) {}
		}, this.sendDelayMs);
	}

	on(event, handler) {
		if (event !== 'message') return this.ws.on(event, handler);
		this.ws.on('message', (msg) => {
			if (Math.random() < this.dropProb) return; // drop
			setTimeout(() => handler(msg), this.recvDelayMs);
		});
	}

	once(event, handler) { return this.on(event, handler); }

	close() { try { this.ws.close(); } catch (_) {} }
}

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
	const payload = { sub: 'chaos-network', iss, aud, rooms: [roomId] };
	return jwt.sign(payload, secret, { algorithm: 'HS256', expiresIn: '5m' });
}

function connectClient(url, timeoutMs = 15000) {
	const WebSocket = require('ws');
	return new Promise((resolve, reject) => {
		const protocols = process.env.SUBPROTOCOL ? [process.env.SUBPROTOCOL] : undefined;
		const ws = new WebSocket(url, protocols, { headers: { origin: 'http://localhost' } });
		const t = setTimeout(() => { try { ws.terminate(); } catch (_) {} reject(new Error('WS connect timeout')); }, timeoutMs);
		ws.once('open', () => { clearTimeout(t); resolve(ws); });
		ws.once('error', (e) => { clearTimeout(t); reject(e); });
	});
}

describe('Chaos: network jitter and packet loss', () => {
	it('signaling completes under 10% packet loss and 50ms jitter', async () => {
		const wsPort = await getFreePort();
		const healthPort = await getFreePort();
		const child = startServer({ wsPort, healthPort });
		try {
			await waitForReady(healthPort, 20000);
			const roomId = `chaos-net-${Date.now()}`;
			const token = makeAuthToken(roomId);
			const qs = token ? `&token=${token}` : '';
			const base = `ws://127.0.0.1:${wsPort}`;
			const raw1 = await connectClient(`${base}/?roomId=${roomId}${qs}`);
			const raw2 = await connectClient(`${base}/?roomId=${roomId}${qs}`);
			const c1 = new JitterWS(raw1, { sendDelayMs: 50, recvDelayMs: 50, dropProb: 0.1 });
			const c2 = new JitterWS(raw2, { sendDelayMs: 50, recvDelayMs: 50, dropProb: 0.1 });

			await new Promise((r) => setTimeout(r, 100));
			const receive = new Promise((resolve, reject) => {
				c2.once('message', (msg) => { try { resolve(JSON.parse(msg.toString())); } catch (e) { reject(e); } });
				raw2.once('error', reject);
			});
			c1.send(JSON.stringify({ type: 'control', action: 'jitter' }));
			const data = await Promise.race([
				receive,
				new Promise((_, rej) => setTimeout(() => rej(new Error('No message under jitter')), 30000)),
			]);
			expect(data).toEqual({ type: 'control', action: 'jitter' });

			c1.close();
			c2.close();
		} finally {
			try { child.kill('SIGTERM'); } catch (_) {}
			try { await waitForExit(child, 5000); } catch (_) {}
		}
	});
});


