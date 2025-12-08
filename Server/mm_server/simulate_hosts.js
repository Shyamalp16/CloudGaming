const axios = require('axios');
const crypto = require('crypto');

const BASE_URL = 'http://localhost:3000';
const HOST_SECRET = 'HELLO-MFS'; // Must match your .env or config
const NUM_HOSTS = 10;
const HEARTBEAT_INTERVAL_MS = 20000; // 20 seconds

// Generate persistent mock hosts
const hosts = Array.from({ length: NUM_HOSTS }, (_, i) => ({
    hostId: `sim-host-${i + 1}-${crypto.randomUUID().slice(0, 8)}`,
    roomId: `room-sim-${i + 1}-${crypto.randomUUID().slice(0, 8)}`,
    region: 'us-east-1',
    status: 'idle',
    gameInfo: {
        name: `Simulated Rig #${i + 1}`,
        specs: 'RTX 4090 (Simulated)'
    }
}));

async function sendHeartbeat(host) {
    try {
        await axios.post(`${BASE_URL}/api/host/heartbeat`, host, {
            headers: {
                'Authorization': `Bearer ${HOST_SECRET}`
            }
        });
        const time = new Date().toISOString().split('T')[1].split('.')[0];
        console.log(`[${time}] Heartbeat sent for ${host.hostId}`);
    } catch (error) {
        console.error(`Failed heartbeat for ${host.hostId}:`, error.message);
    }
}

async function runSimulation() {
    console.log(`Starting simulation with ${NUM_HOSTS} hosts...`);
    console.log(`Target: ${BASE_URL}`);
    console.log(`Interval: ${HEARTBEAT_INTERVAL_MS}ms`);
    console.log('Press Ctrl+C to stop.\n');

    // Initial heartbeats
    await Promise.all(hosts.map(sendHeartbeat));

    // Periodic heartbeats
    setInterval(async () => {
        await Promise.all(hosts.map(sendHeartbeat));
    }, HEARTBEAT_INTERVAL_MS);
}

runSimulation();
