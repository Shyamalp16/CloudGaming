const { spawn } = require('child_process');
const path = require('path');
const net = require('net');
const WebSocket = require('ws');

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

function connect(url, { origin = 'http://localhost', subprotocol } = {}, timeoutMs = 10000) {
	return new Promise((resolve, reject) => {
		const protocols = subprotocol ? [subprotocol] : undefined;
		const ws = new WebSocket(url, protocols, { headers: { origin } });
		const t = setTimeout(() => { try { ws.terminate(); } catch (_) {} reject(new Error('WS connect timeout')); }, timeoutMs);
		ws.once('open', () => { clearTimeout(t); resolve(ws); });
		ws.once('error', (e) => { clearTimeout(t); reject(e); });
	});
}

describe('Server core path coverage', () => {
	it('schema rejection sends control error to sender', async () => {
		const wsPort = await getFreePort();
		const healthPort = await getFreePort();
		const child = startServer({ wsPort, healthPort });
		try {
			await waitForReady(healthPort, 20000);
			const roomId = `paths-schema-${Date.now()}`;
			const base = `ws://127.0.0.1:${wsPort}`;
			const c1 = await connect(`${base}/?roomId=${roomId}`);
			const c2 = await connect(`${base}/?roomId=${roomId}`);
			await new Promise((r) => setTimeout(r, 100));
			const got = new Promise((resolve, reject) => {
				c1.once('message', (m) => { try { resolve(JSON.parse(m.toString())); } catch (e) { reject(e); } });
				c1.once('error', reject);
			});
			c1.send(JSON.stringify({ type: 'bogus' }));
			const msg = await Promise.race([got, new Promise((_, rej) => setTimeout(() => rej(new Error('no schema error')), 10000))]);
			expect(msg).toEqual({ type: 'control', action: 'schema-error' });
			try { c1.close(); } catch (_) {}
			try { c2.close(); } catch (_) {}
		} finally {
			try { child.kill('SIGTERM'); } catch (_) {}
			try { await waitForExit(child, 5000); } catch (_) {}
		}
	});

	it('drops oversized messages', async () => {
		const wsPort = await getFreePort();
		const healthPort = await getFreePort();
		const child = startServer({ wsPort, healthPort, extraEnv: { MESSAGE_MAX_BYTES: '64' } });
		try {
			await waitForReady(healthPort, 20000);
			const roomId = `paths-size-${Date.now()}`;
			const base = `ws://127.0.0.1:${wsPort}`;
			const c1 = await connect(`${base}/?roomId=${roomId}`);
			const c2 = await connect(`${base}/?roomId=${roomId}`);
			await new Promise((r) => setTimeout(r, 100));
			const received = new Promise((resolve) => { c2.once('message', (m) => resolve(m)); });
			c1.send(JSON.stringify({ type: 'control', action: 'x', payload: { blob: 'a'.repeat(512) } }));
			await expect(Promise.race([received, new Promise((_, rej) => setTimeout(() => rej(new Error('no drop')), 3000))])).rejects.toBeDefined();
			try { c1.close(); } catch (_) {}
			try { c2.close(); } catch (_) {}
		} finally {
			try { child.kill('SIGTERM'); } catch (_) {}
			try { await waitForExit(child, 5000); } catch (_) {}
		}
	});

	it('connection rate limiting enforces 1013 on excess', async () => {
		const wsPort = await getFreePort();
		const healthPort = await getFreePort();
		const child = startServer({ wsPort, healthPort, extraEnv: { RATE_LIMIT_CONN_PER_10S: '1' } });
		try {
			await waitForReady(healthPort, 20000);
			const roomId = `paths-rl-${Date.now()}`;
			const base = `ws://127.0.0.1:${wsPort}`;
			const c1 = await connect(`${base}/?roomId=${roomId}`);
			const result = await new Promise((resolve) => {
				const ws = new WebSocket(`${base}/?roomId=${roomId}`, undefined, { headers: { origin: 'http://localhost' } });
				let captured;
				ws.once('open', () => { /* may open then close */ });
				ws.once('close', (code) => { captured = code; resolve(captured); });
				ws.once('error', () => resolve('error'));
				setTimeout(() => resolve('timeout'), 5000);
			});
			expect([1013, 'error']).toContain(result);
			try { c1.close(); } catch (_) {}
		} finally {
			try { child.kill('SIGTERM'); } catch (_) {}
			try { await waitForExit(child, 5000); } catch (_) {}
		}
	});

	it('origin/subprotocol enforcement rejects missing headers and accepts valid', async () => {
		const wsPort = await getFreePort();
		const healthPort = await getFreePort();
		const child = startServer({ wsPort, healthPort, extraEnv: { ALLOWED_ORIGINS: 'http://allowed', SUBPROTOCOL: 'proto1' } });
		try {
			await waitForReady(healthPort, 20000);
			const roomId = `paths-orig-${Date.now()}`;
			const base = `ws://127.0.0.1:${wsPort}`;
			// Missing/forbidden
			const bad = await new Promise((resolve) => {
				const ws = new WebSocket(`${base}/?roomId=${roomId}`, undefined, { headers: { origin: 'http://forbidden' } });
				ws.once('close', (code) => resolve(code));
				ws.once('error', () => resolve('error'));
				setTimeout(() => resolve('timeout'), 5000);
			});
			expect([1008, 'error']).toContain(bad);
			// Valid
			const ok = await connect(`${base}/?roomId=${roomId}`, { origin: 'http://allowed', subprotocol: 'proto1' });
			expect(ok.readyState).toBe(WebSocket.OPEN);
			ok.close();
		} finally {
			try { child.kill('SIGTERM'); } catch (_) {}
			try { await waitForExit(child, 5000); } catch (_) {}
		}
	});

	it('per-connection message rate limit drops excess messages', async () => {
		const wsPort = await getFreePort();
		const healthPort = await getFreePort();
		const child = startServer({ wsPort, healthPort, extraEnv: { RATE_LIMIT_MESSAGES_PER_10S: '1' } });
		try {
			await waitForReady(healthPort, 20000);
			const roomId = `paths-msg-rl-${Date.now()}`;
			const base = `ws://127.0.0.1:${wsPort}`;
			const c1 = await connect(`${base}/?roomId=${roomId}`);
			const c2 = await connect(`${base}/?roomId=${roomId}`);
			await new Promise((r) => setTimeout(r, 100));
			let count = 0;
			const done = new Promise((resolve, reject) => {
				c2.on('message', () => { count += 1; });
				setTimeout(() => resolve(), 1500);
				c2.once('error', reject);
			});
			c1.send(JSON.stringify({ type: 'control', action: 'a' }));
			c1.send(JSON.stringify({ type: 'control', action: 'b' }));
			c1.send(JSON.stringify({ type: 'control', action: 'c' }));
			await done;
			expect(count).toBeLessThanOrEqual(1);
			try { c1.close(); } catch (_) {}
			try { c2.close(); } catch (_) {}
		} finally {
			try { child.kill('SIGTERM'); } catch (_) {}
			try { await waitForExit(child, 5000); } catch (_) {}
		}
	});
});


