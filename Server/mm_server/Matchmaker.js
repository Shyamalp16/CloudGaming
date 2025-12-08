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
