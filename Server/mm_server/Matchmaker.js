const express = require('express');
const cors = require('cors');
const { config } = require('../config');
const { createClient } = require('redis');
const { z } = require('zod')

const app = express();
app.use(cors());
app.use(express.json());

const authenticateHost = (req, res, next) => {
    const authHeader = req.headers.authorization;
    if(!authHeader || !authHeader.startsWith('Bearer ')){
        return res.status(401).json({
            success: false,
            error: 'Unauthorized: Missing or invalid Authorization header'
        });
    }
    const token = authHeader.split(' ')[1];
    if(token !== config.hostSecret){
        return res.status(403).json({
            success: false,
            error: 'Forbidden: Invalid host secret'
        });
    }

    next()
}

app.use('/api/host', authenticateHost);

const HeartbeatSchema = z.object({
    hostId: z.string().uuid().or(z.string().min(1)),
    roomId: z.string().min(1),
    region: z.string().optional(),
    status: z.enum(['idle', 'busy', 'allocated']).optional()
})

app.post('/api/host/heartbeat', async(req, res) => {
    const result = HeartbeatSchema.safeParse(req.body);
    if (!result.success) {
        return res.status(400).json({ error: result.error });
    }
    const { hostId, roomId, region, status } = result.data;
    if( !hostId || !roomId){
        return res.status(400).json({ success: false, error: 'Missing hostId or roomId' });
    }
    const key = `host:${hostId}`;
    const value = JSON.stringify({
        hostId,
        roomId,
        region: region || 'local',
        status: status || 'idle',
        lastHeartbeat: Date.now()
    });

    try{
        const isIdle = (!status || status === 'idle');
        const multi = redisClient.multi()
        multi.set(key, value, {EX:30})

        if(isIdle){
            multi.sAdd('idle_hosts', hostId)
        }else{
            multi.sRem('idle_hosts', hostId)
        }
        await multi.exec()
        res.json({ success: true, ttl: 30})
    }catch (err){
        console.error('Failed to set heartbeat', err);
        res.status(500).json({ success: false, error: 'Failed to set heartbeat' });
    }
})

app.post('/api/match/find', async(req, res) => {
    try{
        const { region } = req.body;
        const rawCandidates = await redisClient.sRandMember('idle_hosts', 10);
        const candidateIds = Array.isArray(rawCandidates) ? rawCandidates : [rawCandidates];
        console.log('Debug - Candidates:', candidateIds);

        if(!candidateIds || candidateIds.length === 0){
            return res.status(404).json({found: false, message: 'No hosts available'})
        }

        for(const hostId of candidateIds){
            const key = `host:${hostId}`
            await redisClient.watch(key);
            const json = await redisClient.get(key);
            
            console.log(`Debug - Host ${hostId} JSON:`, json);

            if(!json) {
                await redisClient.sRem('idle_hosts', hostId)
                await redisClient.unwatch();
                continue;
            }
            let host;
            try {
                host = JSON.parse(json)
            }catch(e){
                await redisClient.unwatch();
                continue;
            }
            const isIdle = host.status === 'idle'
            const regionsMatch = !region || host.region === region;
            
            console.log(`Debug - Match check: isIdle=${isIdle}, regionsMatch=${regionsMatch}, region=${region}, hostRegion=${host.region}`);
            
            if(isIdle && regionsMatch){
                host.status = 'allocated'
                const multi = redisClient.multi().set(key, JSON.stringify(host), {EX: 30}).sRem('idle_hosts', hostId)
                const results = await multi.exec()
                console.log('Debug - Transaction results:', results);
                if(results){
                    return res.json({
                        found: true,
                        roomId: host.roomId,
                        signalingUrl: `ws://localhost:${config.wsPort}`,
                        iceServers: [
                            {
                                urls: "stun:stun.l.google.com:19032"
                            },
                            {
                                urls: "turn:openrelay.metered.ca:80",
                                username: "openrelayproject",
                                credential: "openrelayproject"
                            }
                        ]
                    });
                }
            }else{
                await redisClient.unwatch()
            }
        }
        return res.status(404).json({ found: false, message: 'No hosts available' });
    }catch(err){
        console.error('Match Error:', err)
        res.status(500).json({ error: 'Internal server error'})
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
