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

function connectWithJoinResult(url, timeoutMs = 5000, settleMs = 300) {
	return new Promise((resolve) => {
		const ws = new WebSocket(url);
		let settled = false;
		const done = (result) => { if (!settled) { settled = true; resolve(result); } };
		const to = setTimeout(() => { try { ws.terminate(); } catch (_) {} done({ ok: false, err: new Error('WS connect timeout') }); }, timeoutMs);
		ws.once('open', () => {
			setTimeout(() => {
				clearTimeout(to);
				if (ws.readyState === WebSocket.OPEN) {
					done({ ok: true, ws });
				} else {
					done({ ok: false, err: new Error('Closed after open') });
				}
			}, settleMs);
		});
		ws.once('close', (code) => {
			if (code === 1000) {
				clearTimeout(to);
				done({ ok: false, err: new Error('Room full') });
			}
		});
		ws.once('error', () => {
			clearTimeout(to);
			done({ ok: false, err: new Error('WS error') });
		});
	});
}

describe('Atomic capacity enforcement', () => {
	it('accepts up to capacity and rejects excess concurrent joins', async () => {
		const wsPort = await getFreePort();
		const healthPort = await getFreePort();
		const child = startServer({ wsPort, healthPort });
		try {
			await Promise.race([
				waitForReady(healthPort, 20000),
				waitForExit(child, 20000).then(({ code, signal }) => { throw new Error(`Server exited early: ${code}:${signal}`); }),
			]);
			const roomId = 'cap-room';
			const base = `ws://127.0.0.1:${wsPort}`;
			// Fire off 5 concurrent connection attempts
			const attempts = Array.from({ length: 5 }, () => connectWithJoinResult(`${base}/?roomId=${roomId}`));
			const results = await Promise.allSettled(attempts);
			const successes = results
				.filter(r => r.status === 'fulfilled' && r.value.ok)
				.map(r => r.value.ws);
			// Expect at most 2 successful connections
			expect(successes.length).toBeLessThanOrEqual(2);
			// Close any successful sockets
			await Promise.all(successes.map(ws => new Promise((r) => { try { ws.close(); } catch (_) {} setTimeout(r, 20); })));
		} finally {
			try { child.kill('SIGTERM'); } catch (_) {}
			try { await waitForExit(child, 5000); } catch (_) {}
		}
	});
});


