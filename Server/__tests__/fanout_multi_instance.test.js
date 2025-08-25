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

function connectClient(url, timeoutMs = 10000) {
	return new Promise((resolve, reject) => {
		const ws = new WebSocket(url);
		const t = setTimeout(() => { try { ws.terminate(); } catch (_) {} reject(new Error('WS connect timeout')); }, timeoutMs);
		ws.once('open', () => { clearTimeout(t); resolve(ws); });
		ws.once('error', (e) => { clearTimeout(t); reject(e); });
	});
}

describe('Pub/Sub fanout across instances', () => {
	it('delivers messages between clients on different server instances', async () => {
		const wsPort1 = await getFreePort();
		const wsPort2 = await getFreePort();
		const healthPort1 = await getFreePort();
		const healthPort2 = await getFreePort();
		const s1 = startServer({ wsPort: wsPort1, healthPort: healthPort1 });
		const s2 = startServer({ wsPort: wsPort2, healthPort: healthPort2 });
		try {
			await Promise.all([
				waitForReady(healthPort1, 20000),
				waitForReady(healthPort2, 20000),
			]);
			const roomId = `fanout-${Date.now()}`;
			const c1 = await connectClient(`ws://127.0.0.1:${wsPort1}/?roomId=${roomId}`);
			const c2 = await connectClient(`ws://127.0.0.1:${wsPort2}/?roomId=${roomId}`);
			const received = new Promise((resolve, reject) => {
				c2.once('message', (msg) => {
					try { resolve(JSON.parse(msg.toString())); } catch (e) { reject(e); }
				});
				c2.once('error', reject);
			});
			await new Promise((r) => setTimeout(r, 150));
			c1.send(JSON.stringify({ type: 'control', action: 'cross', payload: { ok: true } }));
			const data = await Promise.race([
				received,
				new Promise((_, rej) => setTimeout(() => rej(new Error('No fanout')), 20000)),
			]);
			expect(data).toEqual({ type: 'control', action: 'cross', payload: { ok: true } });
			try { c1.close(); } catch (_) {}
			try { c2.close(); } catch (_) {}
		} finally {
			try { s1.kill('SIGTERM'); } catch (_) {}
			try { s2.kill('SIGTERM'); } catch (_) {}
			try { await waitForExit(s1, 5000); } catch (_) {}
			try { await waitForExit(s2, 5000); } catch (_) {}
		}
	});
});


