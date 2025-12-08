const express = require('express');
const cors = require('cors');
const { config } = require('../config');
const { createClient } = require('redis');

const app = express();
app.use(cors());
app.use(express.json());

app.post('/api/host/heartbeat', async(req, res) => {
    const { hostId, roomId, region, status } = req.body;
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
        await redisClient.set(key, value, { EX: 30 });
        res.json({ success: true, ttl: 30 });
    }catch (err){
        console.error('Failed to set heartbeat', err);
        res.status(500).json({ success: false, error: 'Failed to set heartbeat' });
    }
})

app.post('/api/match/find', async(req, res) => {
    try{
        const { region } = req.body;
        const keys = await redisClient.keys('host:*');
        if(keys.length === 0){
            return res.status(404).json({ found:false, message:'No hosts available'});
        }
        const hostsJSON = await redisClient.mGet(keys);
        let match = null;
        for(const json of hostsJSON){
            if(!json) continue;
            let host;
            try{
                host = JSON.parse(json)
            }catch(e){
                continue;
            }

            const isIdle = host.status === 'idle'
            const regionsMatch = !region || host.region === region;

            if(isIdle && regionsMatch){
                host.status = 'allocated'
                await redisClient.set(`host:${host.hostId}`, JSON.stringify(host), {EX: 30});
                match = host;
                break;
            }
        }
        if(match){
            return res.json({
                found: true,
                roomId: match.roomId,
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
        }else{
            return res.status(404).json({ found: false, message: `All hosts are busy`});
        }
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
