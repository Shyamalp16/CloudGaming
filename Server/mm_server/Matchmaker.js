const express = require('express');
const cors = require('cors');
const { randomUUID } = require('crypto');
const { config } = require('../config');
const { createClient } = require('redis');
const { z } = require('zod');

const app = express();
app.use(cors());
app.use(express.json());

// Simple structured logger
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

// Request ID middleware (honor incoming header if present)
app.use((req, res, next) => {
	const headerId = req.headers['x-request-id'];
	const reqId = (typeof headerId === 'string' && headerId.trim()) ? headerId.trim() : randomUUID();
	req.id = reqId;
	res.setHeader('x-request-id', reqId);
	next();
});

// Helper to format zod errors consistently
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

async function pruneStaleIdleHosts() {
	const stale = [];
	const ids = await redisClient.sMembers('idle_hosts');
	for (const id of ids) {
		const ttl = await redisClient.ttl(`host:${id}`);
		if (ttl === -2) {
			stale.push(id);
		}
	}
	if (stale.length > 0) {
		await redisClient.sRem('idle_hosts', stale);
		log('info', 'Pruned stale idle hosts', { staleCount: stale.length, ids: stale });
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
	const value = JSON.stringify({
		hostId,
		roomId,
		region: region || 'local',
		status: availableSlots > 0 ? (status || 'idle') : (status || 'busy'),
		capacity,
		availableSlots,
		lastHeartbeat: Date.now(),
	});

	try {
		const isIdle = availableSlots > 0;
		const multi = redisClient.multi();
		multi.set(key, value, { EX: 30 });

		if (isIdle) {
			multi.sAdd('idle_hosts', hostId);
		} else {
			multi.sRem('idle_hosts', hostId);
		}
		await multi.exec();
		log('info', 'Heartbeat accepted', { requestId: req.id, hostId, status: status || 'idle' });
		res.json({ success: true, ttl: 30 });
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

// Observability: TTL view for idle hosts
app.get('/api/hosts/ttl', async (req, res) => {
	try {
		await pruneStaleIdleHosts();
		const hostIds = await redisClient.sMembers('idle_hosts');
		const ttlEntries = [];
		for (const id of hostIds) {
			const ttl = await redisClient.ttl(`host:${id}`);
			if (ttl === -2) {
				await redisClient.sRem('idle_hosts', id);
				continue;
			}
			ttlEntries.push({ hostId: id, ttlSeconds: ttl });
		}
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

		// Candidate gathering with region-aware weighting
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
		for (const currentHostId of candidateIds) {
			const key = `host:${currentHostId}`;
			const json = await redisClient.get(key);
			if (!json) {
				await redisClient.sRem('idle_hosts', currentHostId);
				continue;
			}
			let host;
			try {
				host = JSON.parse(json);
			} catch (_) {
				await redisClient.sRem('idle_hosts', currentHostId);
				continue;
			}
			const capacity = Math.max(1, host.capacity || 1);
			const availableSlots = Math.max(0, Math.min(capacity, (typeof host.availableSlots === 'number') ? host.availableSlots : capacity));
			if (availableSlots <= 0) continue;
			const regionsMatch = !region || host.region === region;
			const weight = regionsMatch ? 5 : 1;
			candidates.push({ hostId: currentHostId, host: { ...host, availableSlots, capacity }, weight });
		}

		if (candidates.length === 0) {
			return res.status(404).json({ found: false, message: 'No hosts available' });
		}

		// Prefer region-matching candidates if present
		const regionPreferred = region ? candidates.filter(c => c.host.region === region) : candidates;
		const selectionPool = (region && regionPreferred.length > 0) ? regionPreferred : candidates;
		log('info', 'Match candidates prepared', { requestId: req.id, total: candidates.length, selectionPool: selectionPool.length, region });

		// Attempt allocations until success or exhaustion
		const pool = [...selectionPool];
		while (pool.length > 0) {
			const pick = weightedPick(pool);
			if (!pick) break;
			const { hostId: currentHostId } = pick;
			const key = `host:${currentHostId}`;

			await redisClient.watch(key);
			const json = await redisClient.get(key);
			if (!json) {
				await redisClient.sRem('idle_hosts', currentHostId);
				await redisClient.unwatch();
				pool.splice(pool.indexOf(pick), 1);
				continue;
			}
			let host;
			try { host = JSON.parse(json); } catch (_) {
				await redisClient.unwatch();
				pool.splice(pool.indexOf(pick), 1);
				continue;
			}

			const capacity = Math.max(1, host.capacity || 1);
			const availableSlotsCurrent = Math.max(0, Math.min(capacity, (typeof host.availableSlots === 'number') ? host.availableSlots : capacity));
			const isIdle = availableSlotsCurrent > 0;
			const regionsMatch = !region || host.region === region;
			log('debug', 'Match check', { requestId: req.id, hostId: currentHostId, isIdle, regionsMatch, region, hostRegion: host.region, availableSlots: availableSlotsCurrent, capacity });

			if (isIdle && regionsMatch) {
				const remainingSlots = Math.max(0, availableSlotsCurrent - 1);
				host.availableSlots = remainingSlots;
				host.capacity = capacity;
				host.status = remainingSlots > 0 ? 'idle' : 'busy';
				const multi = redisClient.multi()
					.set(key, JSON.stringify(host), { EX: 30 });
				if (remainingSlots > 0) {
					multi.sAdd('idle_hosts', currentHostId);
				} else {
					multi.sRem('idle_hosts', currentHostId);
				}

				const results = await multi.exec();
				log('info', 'Transaction results', { requestId: req.id, hostId: currentHostId, results, remainingSlots });

				if (results) {
					return res.json({
						found: true,
						roomId: host.roomId,
						signalingUrl: `ws://localhost:${config.wsPort}`,
						iceServers: [
							{ urls: "stun:stun.l.google.com:19302" },
							{ urls: "turn:openrelay.metered.ca:80", username: "openrelayproject", credential: "openrelayproject" }
						]
					});
				}
			}

			await redisClient.unwatch();
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
    try{
        await redisClient.connect();
        console.log('Connected to Redis');

        app.listen(config.mmPort, () => {
            console.log(`Matchmaker server is running on port ${config.mmPort}`);
        });
    }
    catch (error) {
        console.error('Failed to start matchmaker server', error);
        process.exit(1);
    }
}

startServer();
