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

jest.setTimeout(30000);

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

function connectClient(url) {
	return new Promise((resolve, reject) => {
		const ws = new WebSocket(url);
		ws.once('open', () => resolve(ws));
		ws.once('error', reject);
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
			const c1 = await connectClient(`${base}/?roomId=${roomId}`);
			const c2 = await connectClient(`${base}/?roomId=${roomId}`);

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

			c1.send(JSON.stringify({ type: 'control', action: 'test', payload: { ok: true } }));
			const data = await received;
			expect(data).toEqual({ type: 'control', action: 'test', payload: { ok: true } });

			c1.close();
			c2.close();
		} finally {
			try { child.kill('SIGTERM'); } catch (_) {}
			try { await waitForExit(child, 5000); } catch (_) {}
		}
	});
});


