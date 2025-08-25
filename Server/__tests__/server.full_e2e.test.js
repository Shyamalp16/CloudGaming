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
	const payload = { sub: 'full-e2e', iss, aud, rooms: [roomId] };
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

describe('Full E2E signaling', () => {
	it('offer/answer/candidate flow with latency assertions', async () => {
		const wsPort = await getFreePort();
		const healthPort = await getFreePort();
		const child = startServer({ wsPort, healthPort });
		try {
			await Promise.race([
				waitForReady(healthPort, 20000),
				waitForExit(child, 20000).then(({ code, signal }) => { throw new Error(`Server exited early: ${code}:${signal}`); }),
			]);
			const roomId = `e2e-full-${Date.now()}`;
			const token = makeAuthToken(roomId);
			const qs = token ? `&token=${token}` : '';
			const base = `ws://127.0.0.1:${wsPort}`;
			const c1 = await connectClient(`${base}/?roomId=${roomId}${qs}`);
			const c2 = await connectClient(`${base}/?roomId=${roomId}${qs}`);

			// Simulate signaling messages flowing through server
			const receiveOnC2 = (type) => new Promise((resolve, reject) => {
				c2.once('message', (msg) => {
					try { const data = JSON.parse(msg.toString()); resolve(data); } catch (e) { reject(e); }
				});
				c2.once('error', reject);
			});

			await new Promise((r) => setTimeout(r, 100));
			const tOffer = Date.now();
			c1.send(JSON.stringify({ type: 'offer', sdp: 'v=0' }));
			const offerData = await Promise.race([
				receiveOnC2('offer'),
				new Promise((_, rej) => setTimeout(() => rej(new Error('offer timeout')), 20000)),
			]);
			expect(offerData.type).toBe('offer');
			expect(Date.now() - tOffer).toBeLessThan(500);

			const tAnswer = Date.now();
			c2.send(JSON.stringify({ type: 'answer', sdp: 'v=0' }));
			const answerData = await new Promise((resolve, reject) => {
				c1.once('message', (msg) => { try { resolve(JSON.parse(msg.toString())); } catch (e) { reject(e); } });
				c1.once('error', reject);
			});
			expect(answerData.type).toBe('answer');
			expect(Date.now() - tAnswer).toBeLessThan(500);

			const tCand = Date.now();
			c1.send(JSON.stringify({ type: 'candidate', candidate: 'cand', sdpMid: '0', sdpMLineIndex: 0 }));
			const candData = await receiveOnC2('candidate');
			expect(candData.type).toBe('candidate');
			expect(Date.now() - tCand).toBeLessThan(500);

			try { c1.close(); } catch (_) {}
			try { c2.close(); } catch (_) {}
		} finally {
			try { child.kill('SIGTERM'); } catch (_) {}
			try { await waitForExit(child, 5000); } catch (_) {}
		}
	});

	it('disconnect and reconnect resumes signaling', async () => {
		const wsPort = await getFreePort();
		const healthPort = await getFreePort();
		const child = startServer({ wsPort, healthPort });
		try {
			await waitForReady(healthPort, 20000);
			const roomId = `e2e-recon-${Date.now()}`;
			const token = makeAuthToken(roomId);
			const qs = token ? `&token=${token}` : '';
			const base = `ws://127.0.0.1:${wsPort}`;
			let c1 = await connectClient(`${base}/?roomId=${roomId}${qs}`);
			let c2 = await connectClient(`${base}/?roomId=${roomId}${qs}`);

			await new Promise((r) => setTimeout(r, 100));
			c1.send(JSON.stringify({ type: 'control', action: 'initial' }));
			await new Promise((resolve, reject) => {
				c2.once('message', () => resolve());
				c2.once('error', reject);
			});

			// Disconnect c2 and reconnect
			try { c2.close(); } catch (_) {}
			await new Promise((r) => setTimeout(r, 100));
			c2 = await connectClient(`${base}/?roomId=${roomId}${qs}`);
			await new Promise((r) => setTimeout(r, 100));
			const t = Date.now();
			c1.send(JSON.stringify({ type: 'control', action: 'resume' }));
			const msg = await new Promise((resolve, reject) => {
				c2.once('message', (m) => { try { resolve(JSON.parse(m.toString())); } catch (e) { reject(e); } });
				c2.once('error', reject);
			});
			expect(msg).toEqual({ type: 'control', action: 'resume' });
			expect(Date.now() - t).toBeLessThan(1000);

			try { c1.close(); } catch (_) {}
			try { c2.close(); } catch (_) {}
		} finally {
			try { child.kill('SIGTERM'); } catch (_) {}
			try { await waitForExit(child, 5000); } catch (_) {}
		}
	});
});


