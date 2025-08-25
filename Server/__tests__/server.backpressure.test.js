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

async function waitForReady(healthPort, timeoutMs = 40000) {
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

describe('Backpressure close path', () => {
	it('closes client when bufferedAmount exceeds threshold', async () => {
		const wsPort = await getFreePort();
		const healthPort = await getFreePort();
		const child = startServer({ wsPort, healthPort, extraEnv: { BACKPRESSURE_CLOSE_THRESHOLD_BYTES: '0', MESSAGE_MAX_BYTES: String(1024 * 1024) } });
		try {
			// Be resilient to slow boots: race readiness with unexpected exit
			await Promise.race([
				waitForReady(healthPort, 40000),
				waitForExit(child, 40000).then(({ code, signal }) => { throw new Error(`Server exited early: ${code}:${signal}`); }),
			]);
			const roomId = `bp-${Date.now()}`;
			const base = `ws://127.0.0.1:${wsPort}`;
			const a = await connect(`${base}/?roomId=${roomId}`);
			const b = await connect(`${base}/?roomId=${roomId}`);
			// Make receiver artificially slow by pausing its socket (approximation)
			try { b._socket.pause(); } catch (_) {}
			await new Promise((r) => setTimeout(r, 100));
			// Send enough large messages to grow server's send buffer to b
			const payload = JSON.stringify({ type: 'control', action: 'spam', payload: { blob: 'x'.repeat(65536) } });
			for (let i = 0; i < 600; i++) {
				try { a.send(payload); } catch (_) {}
			}
			const closed = await new Promise((resolve, reject) => {
				b.once('close', (code) => resolve(code));
				setTimeout(() => reject(new Error('no backpressure close')), 30000);
			});
			expect([1013, 1006, 1001]).toContain(closed);
		} finally {
			try { child.kill('SIGTERM'); } catch (_) {}
			try { await waitForExit(child, 5000); } catch (_) {}
		}
	});
});


