const express = require('express');
const { randomUUID } = require('crypto');
const { config } = require('../config');
const { createClient } = require('redis');
const { z } = require('zod');
const https = require('https');

// Catch anything that would silently kill the process and log it first.
process.on('uncaughtException',   (err) => console.error('[FATAL] uncaughtException:',   err));
process.on('unhandledRejection',  (reason) => console.error('[FATAL] unhandledRejection:', reason));

// ─── Metered TURN helper ─────────────────────────────────────────────────────
// Fetches short-lived ICE server credentials from the Metered API.
// Cached briefly so simultaneous matches do not trigger duplicate external API calls.
// Falls back to Google STUN-only if Metered is not configured.
let iceServerCache = null;
let iceServerCacheExpiresAt = 0;
let iceServerRequest = null;

async function getIceServers() {
	const { domain, apiKey, expirySeconds } = config.metered;
	const fallback = [{ urls: 'stun:stun.l.google.com:19302' }];

	if (!domain || !apiKey) {
		return fallback;
	}
	if (iceServerCache && Date.now() < iceServerCacheExpiresAt) return iceServerCache;
	if (iceServerRequest) return iceServerRequest;

	iceServerRequest = (async () => {
	try {
		// Step 1: create an expiring credential
		const createRes = await fetchJson(
			`https://${domain}.metered.live/api/v1/turn/credential?secretKey=${apiKey}`,
			'POST',
			{ expiryInSeconds: expirySeconds }
		);
		if (!createRes || !createRes.apiKey) {
			log('warn', 'Metered credential creation returned no apiKey', { createRes });
			return fallback;
		}

		// Step 2: fetch the full iceServers array using the one-time apiKey
		const iceServers = await fetchJson(
			`https://${domain}.metered.live/api/v1/turn/credentials?apiKey=${createRes.apiKey}`,
			'GET'
		);
		if (!Array.isArray(iceServers) || iceServers.length === 0) {
			log('warn', 'Metered returned empty iceServers', { iceServers });
			return fallback;
		}

		// Always include a STUN server alongside the TURN servers
		iceServerCache = [{ urls: 'stun:stun.l.google.com:19302' }, ...iceServers];
		const cacheSeconds = Math.max(30, Math.min(600, expirySeconds - 60));
		iceServerCacheExpiresAt = Date.now() + cacheSeconds * 1000;
		return iceServerCache;
	} catch (err) {
		log('error', 'Failed to fetch Metered TURN credentials', { error: String(err && err.message || err) });
		return fallback;
	}
	})();
	try {
		return await iceServerRequest;
	} finally {
		iceServerRequest = null;
	}
}

function fetchJson(url, method, body) {
	return new Promise((resolve, reject) => {
		const parsed = new URL(url);
		const data = body ? JSON.stringify(body) : null;
		const options = {
			hostname: parsed.hostname,
			path: parsed.pathname + parsed.search,
			method: method || 'GET',
			headers: {
				'Content-Type': 'application/json',
				...(data ? { 'Content-Length': Buffer.byteLength(data) } : {}),
			},
		};
		const req = https.request(options, (res) => {
			let raw = '';
			res.on('data', (chunk) => {
				raw += chunk;
				if (raw.length > 1024 * 1024) req.destroy(new Error('Response too large'));
			});
			res.on('end', () => {
				if (res.statusCode < 200 || res.statusCode >= 300) {
					reject(new Error(`HTTP ${res.statusCode}: ${raw.slice(0, 200)}`));
					return;
				}
				try { resolve(JSON.parse(raw)); }
				catch (e) { reject(new Error(`JSON parse failed: ${raw.slice(0, 200)}`)); }
			});
		});
		req.on('error', reject);
		req.setTimeout(5000, () => req.destroy(new Error('Request timed out')));
		if (data) req.write(data);
		req.end();
	});
}
// ─────────────────────────────────────────────────────────────────────────────

const app = express();
app.enable('trust proxy');

// Hard-wire CORS headers on every response.  This MUST be the very first
// middleware so that OPTIONS preflights are answered before anything else
// (including any error paths) can run.
app.use((req, res, next) => {
	res.setHeader('Access-Control-Allow-Origin', '*');
	res.setHeader('Access-Control-Allow-Methods', 'GET, POST, OPTIONS');
	res.setHeader('Access-Control-Allow-Headers', 'Content-Type, Authorization, X-Request-Id');
	res.setHeader('Access-Control-Max-Age', '86400');
	if (req.method === 'OPTIONS') return res.sendStatus(204);
	next();
});

// Parse JSON request bodies for heartbeat and match APIs.
app.use(express.json());

// ── Health / readiness probes ─────────────────────────────────────────────────
// Railway (and other platforms) hit these before routing real traffic.
// Respond immediately so the container is never killed for a missing probe.
app.get('/healthz',  (_req, res) => res.sendStatus(200));
app.get('/readyz',   (_req, res) => res.sendStatus(redisClient.isReady ? 200 : 503));
app.get('/health',   (_req, res) => res.sendStatus(200));
app.get('/', (_, res) => res.send('ok'));
// ─────────────────────────────────────────────────────────────────────────────

function log(level, message, meta) {
	const entry = { level, message, ...(meta || {}) };
	if (level === 'error') {
		console.error(entry);
	} else if (level === 'warn') {
		console.warn(entry);
	} else {
		console.log(entry);
	}
}

app.use((req, res, next) => {
	const headerId = req.headers['x-request-id'];
	const reqId = (typeof headerId === 'string' && headerId.trim()) ? headerId.trim() : randomUUID();
	req.id = reqId;
	res.setHeader('x-request-id', reqId);
	next();
});

function formatZodIssues(zodError) {
	return zodError.errors.map((e) => ({
		path: e.path.join('.'),
		message: e.message,
		code: e.code,
	}));
}

const authenticateHost = (req, res, next) => {
	const authHeader = req.headers.authorization;
	if (!authHeader || !authHeader.startsWith('Bearer ')) {
		return res.status(401).json({
			success: false,
			error: 'Unauthorized: Missing or invalid Authorization header',
		});
	}
	const token = authHeader.split(' ')[1];
	const allowedSecrets = [config.hostSecret, config.hostSecretPrevious].filter(Boolean);
	if (!allowedSecrets.includes(token)) {
		return res.status(403).json({
			success: false,
			error: 'Forbidden: Invalid host secret',
		});
	}

	next();
};

app.use('/api/host', authenticateHost);

const HeartbeatSchema = z.object({
	hostId: z.string().uuid().or(z.string().min(1)),
	roomId: z.string().min(1),
	region: z.string().optional(),
	status: z.enum(['idle', 'busy', 'allocated']).optional(),
	capacity: z.number().int().positive().optional(),
	availableSlots: z.number().int().nonnegative().optional(),
});

const MatchFindSchema = z.object({
	region: z.string().optional(),
	hostId: z.string().optional(),
});

function weightedPick(items) {
	const total = items.reduce((sum, item) => sum + (item.weight || 1), 0);
	if (total <= 0) return null;
	let r = Math.random() * total;
	for (const item of items) {
		r -= (item.weight || 1);
		if (r <= 0) return item;
	}
	return null;
}

const HEARTBEAT_SCRIPT = `
local host = cjson.decode(ARGV[1])
local capacity = tonumber(host.capacity) or 1
local available = tonumber(host.availableSlots) or capacity
local reserved = tonumber(redis.call('GET', KEYS[3]) or '0')
available = math.max(0, math.min(capacity, available - reserved))
host.availableSlots = available
host.status = available > 0 and 'idle' or 'busy'
redis.call('SET', KEYS[1], cjson.encode(host), 'EX', ARGV[3])
if available > 0 then
  redis.call('SADD', KEYS[2], ARGV[2])
else
  redis.call('SREM', KEYS[2], ARGV[2])
end
return available
`;

const CLAIM_HOST_SCRIPT = `
local raw = redis.call('GET', KEYS[1])
if not raw then
  redis.call('SREM', KEYS[2], ARGV[1])
  return nil
end
local host = cjson.decode(raw)
local capacity = tonumber(host.capacity) or 1
local available = tonumber(host.availableSlots) or capacity
if available <= 0 then
  redis.call('SREM', KEYS[2], ARGV[1])
  return nil
end
available = available - 1
host.capacity = capacity
host.availableSlots = available
host.status = available > 0 and 'idle' or 'busy'
redis.call('INCR', KEYS[3])
redis.call('EXPIRE', KEYS[3], ARGV[3])
redis.call('SET', KEYS[1], cjson.encode(host), 'EX', ARGV[2])
if available > 0 then
  redis.call('SADD', KEYS[2], ARGV[1])
else
  redis.call('SREM', KEYS[2], ARGV[1])
end
return cjson.encode(host)
`;

async function pruneStaleIdleHosts() {
	try {
		const ids = await redisClient.sMembers('idle_hosts');
		if (ids.length === 0) return;
		const multi = redisClient.multi();
		ids.forEach((id) => multi.ttl(`host:${id}`));
		const ttls = await multi.exec();
		const stale = ids.filter((_, index) => ttls[index] === -2);
		if (stale.length > 0) {
			await redisClient.sRem('idle_hosts', stale);
			log('info', 'Pruned stale idle hosts', { staleCount: stale.length, ids: stale });
		}
	} catch (error) {
		log('error', 'Failed to prune stale hosts', { error: String(error && error.message || error) });
	}
}

app.post('/api/host/heartbeat', async(req, res) => {
	const result = HeartbeatSchema.safeParse(req.body);
	if (!result.success) {
		return res.status(400).json({
			success: false,
			error: 'Validation failed',
			issues: formatZodIssues(result.error),
		});
	}
	const { hostId, roomId, region, status } = result.data;
	const capacity = Math.max(1, result.data.capacity || 1);
	let availableSlots = result.data.availableSlots;
	if (availableSlots === undefined || availableSlots === null) {
		availableSlots = capacity;
	}
	availableSlots = Math.max(0, Math.min(capacity, availableSlots));
	if (!hostId || !roomId) {
		return res.status(400).json({ success: false, error: 'Missing hostId or roomId' });
	}
	const key = `host:${hostId}`;
	const host = {
		hostId,
		roomId,
		region: region || 'local',
		status: availableSlots > 0 ? (status || 'idle') : (status || 'busy'),
		capacity,
		availableSlots,
		lastHeartbeat: Date.now(),
	};

	try {
		await redisClient.eval(HEARTBEAT_SCRIPT, {
			keys: [key, 'idle_hosts', `host_reservations:${hostId}`],
			arguments: [JSON.stringify(host), hostId, String(config.hostTtlSeconds)],
		});
		log('info', 'Heartbeat accepted', { requestId: req.id, hostId, status: status || 'idle' });
		res.json({ success: true, ttl: config.hostTtlSeconds });
	} catch (err) {
		log('error', 'Failed to set heartbeat', { requestId: req.id, error: String(err && err.message || err) });
		res.status(500).json({ success: false, error: 'Failed to set heartbeat' });
	}
});

app.get('/api/hosts', async (req, res) => {
	try {
		const hostIds = await redisClient.sMembers('idle_hosts');
		if (hostIds.length === 0) {
			return res.json([]);
		}
		const hostKeys = hostIds.map(id => `host:${id}`);
		const hostsJSON = await redisClient.mGet(hostKeys);
		const hosts = hostsJSON
			.filter(json => json !== null)
			.map(json => JSON.parse(json));
		res.json(hosts);
	} catch (error) {
		log('error', 'Failed to list hosts', { requestId: req.id, error: String(error && error.message || error) });
		res.status(500).json({ error: 'Internal server error' });
	}
});

app.get('/api/hosts/ttl', async (req, res) => {
	try {
		await pruneStaleIdleHosts();
		const hostIds = await redisClient.sMembers('idle_hosts');
		const multi = redisClient.multi();
		hostIds.forEach((id) => multi.ttl(`host:${id}`));
		const ttls = hostIds.length > 0 ? await multi.exec() : [];
		const stale = [];
		const ttlEntries = hostIds.flatMap((id, index) => {
			if (ttls[index] === -2) { stale.push(id); return []; }
			return [{ hostId: id, ttlSeconds: ttls[index] }];
		});
		if (stale.length > 0) await redisClient.sRem('idle_hosts', stale);
		res.json(ttlEntries);
	} catch (error) {
		log('error', 'Failed to fetch host TTLs', { requestId: req.id, error: String(error && error.message || error) });
		res.status(500).json({ error: 'Internal server error' });
	}
});

app.post('/api/match/find', async(req, res) => {
	const parsed = MatchFindSchema.safeParse(req.body || {});
	if (!parsed.success) {
		return res.status(400).json({
			success: false,
			error: 'Validation failed',
			issues: formatZodIssues(parsed.error),
		});
	}
	const { region, hostId: requestedHostId } = parsed.data;

	try {
		await pruneStaleIdleHosts();

		const sampleSize = requestedHostId ? 1 : 50;
		const rawCandidates = requestedHostId
			? [requestedHostId]
			: await redisClient.sRandMember('idle_hosts', sampleSize);
		const candidateIds = rawCandidates
			? (Array.isArray(rawCandidates) ? rawCandidates : [rawCandidates])
			: [];

		if (!candidateIds || candidateIds.length === 0) {
			return res.status(404).json({ found: false, message: 'No hosts available' });
		}

		const candidates = [];
		const candidateJson = await redisClient.mGet(candidateIds.map((id) => `host:${id}`));
		const invalidIds = [];
		for (let index = 0; index < candidateIds.length; index++) {
			const currentHostId = candidateIds[index];
			const json = candidateJson[index];
			if (!json) {
				invalidIds.push(currentHostId);
				continue;
			}
			let host;
			try {
				host = JSON.parse(json);
			} catch (_) {
				invalidIds.push(currentHostId);
				continue;
			}
			const capacity = Math.max(1, host.capacity || 1);
			const availableSlots = Math.max(0, Math.min(capacity, (typeof host.availableSlots === 'number') ? host.availableSlots : capacity));
			if (availableSlots <= 0) continue;
			const regionsMatch = !region || host.region === region;
			const weight = regionsMatch ? 5 : 1;
			candidates.push({ hostId: currentHostId, host: { ...host, availableSlots, capacity }, weight });
		}
		if (invalidIds.length > 0) await redisClient.sRem('idle_hosts', invalidIds);

		if (candidates.length === 0) {
			return res.status(404).json({ found: false, message: 'No hosts available' });
		}

		const regionPreferred = region ? candidates.filter(c => c.host.region === region) : candidates;
		const selectionPool = (region && regionPreferred.length > 0) ? regionPreferred : candidates;
		log('info', 'Match candidates prepared', { requestId: req.id, total: candidates.length, selectionPool: selectionPool.length, region });

		const pool = [...selectionPool];
		while (pool.length > 0) {
			const pick = weightedPick(pool);
			if (!pick) break;
			const { hostId: currentHostId } = pick;
			const claimedJson = await redisClient.eval(CLAIM_HOST_SCRIPT, {
				keys: [`host:${currentHostId}`, 'idle_hosts', `host_reservations:${currentHostId}`],
				arguments: [currentHostId, String(config.hostTtlSeconds), String(config.allocationReservationSeconds)],
			});
			if (claimedJson) {
				const host = JSON.parse(claimedJson);
				const iceServers = await getIceServers();
				return res.json({ found: true, roomId: host.roomId, iceServers });
			}
			log('info', 'Allocation race detected, retrying host', { requestId: req.id, hostId: currentHostId });
			pool.splice(pool.indexOf(pick), 1);
		}

		return res.status(404).json({ found: false, message: 'No available hosts found matching criteria' });
	} catch (err) {
		log('error', 'Match Error', { requestId: req.id, error: String(err && err.message || err) });
		res.status(500).json({ error: 'Internal server error' });
	}
});

function createRedis(urlString){
    return createClient({
        url: urlString,
        socket: {
            reconnectStrategy: (retries) => {
                const delay = Math.min(1000 + retries * 50, 5000);
                return delay;
            }
        }
    })
}

const redisClient = createRedis(config.redisUrl);
redisClient.on('error', (err) => console.error('Redis Client Error', err));

async function startServer(){
    // Bind to PORT first so Railway's health check passes immediately.
    // Redis connection happens after — a slow Redis startup no longer kills the container.
    const port = process.env.PORT || config.mmPort;
    await new Promise((resolve, reject) => {
        app.listen(port, '0.0.0.0', (err) => {
            if (err) return reject(err);
            console.log(`Matchmaker server is running on port ${port} (0.0.0.0)`);
            resolve();
        });
    });

    try {
        await redisClient.connect();
        console.log('Connected to Redis');
    } catch (error) {
        console.error('Failed to connect to Redis — retrying in background', error);
        // Don't exit — the redis client's reconnectStrategy will keep retrying
    }

    try {
        await pruneStaleIdleHosts();
    } catch (_) {}
    setInterval(() => pruneStaleIdleHosts().catch(e =>
        console.error('[pruneInterval] error:', e)), 10000);
}

startServer().catch(err => {
    console.error('[FATAL] startServer crashed:', err);
    process.exit(1);
});
