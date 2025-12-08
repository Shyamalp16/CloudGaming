const axios = require('axios');

const BASE_URL = 'http://localhost:3000'; 

async function testMatchmaker() {
    console.log('--- Starting Matchmaker Test ---');
    const hostPayload = {
        hostId: 'test-host-123',
        roomId: 'room-abc-789',
        region: 'us-east-1',
        status: 'idle'
    };
    try {
        console.log('1. Registering Host...');
        await axios.post(`${BASE_URL}/api/host/heartbeat`, hostPayload);
        console.log('   Host registered successfully.');
    } catch (error) {
        console.error('   Failed to register host:', error.message);
        return;
    }
    try {
        console.log('\n2. Finding Match...');
        const response = await axios.post(`${BASE_URL}/api/match/find`, {
            region: 'us-east-1'
        });
        const data = response.data;
        if (data.found) {
            console.log('   Match Found!');
            console.log(`   Room ID: ${data.roomId}`);
            console.log('\n3. Verifying ICE Servers...');
            if (data.iceServers && data.iceServers.length > 0) {
                console.log('   Received ICE Servers:', JSON.stringify(data.iceServers, null, 2)); 
                const hasOpenRelay = JSON.stringify(data.iceServers).includes('openrelay');
                if (hasOpenRelay) {
                     console.log('   ✅ SUCCESS: OpenRelay credentials present.');
                } else {
                     console.log('   ⚠️ WARNING: OpenRelay credentials NOT found.');
                }
            } else {
                console.log('   ❌ FAILED: No ICE servers returned.');
            }
        } else {
            console.log('   Match NOT found (Unexpected).');
        }
    } catch (error) {
        if (error.response) {
             console.log('   Server responded with error:', error.response.status, error.response.data);
        } else {
             console.error('   Request failed:', error.message);
        }
    }
}

testMatchmaker();