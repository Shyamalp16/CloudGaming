const axios = require('axios');

const BASE_URL = 'http://localhost:3000';
const HOST_SECRET = 'HELLO-MFS';

async function testMatchmaker() {
    console.log('--- Starting Matchmaker Test ---');

    // Host with capacity/slots to reflect recent changes
    const hostPayload = {
        hostId: 'test-host-123',
        roomId: 'room-abc-789',
        region: 'us-east-1',
        status: 'idle',
        capacity: 2,
        availableSlots: 2
    };
    
    try {
        console.log('1. Registering Host (with capacity/slots)...');
        await axios.post(`${BASE_URL}/api/host/heartbeat`, hostPayload, {
            headers: {
                'Authorization': `Bearer ${HOST_SECRET}`
            }
        });
        console.log('   Host registered successfully.');
    } catch (error) {
        console.error('   Failed to register host:', error.response?.data || error.message);
        return;
    }
    try {
        console.log('\n2. Finding Match (region us-east-1)...');
        const response = await axios.post(`${BASE_URL}/api/match/find`, {
            region: 'us-east-1'
        });
        const data = response.data;
        if (data.found) {
            console.log('   Match Found!');
            console.log(`   Room ID: ${data.roomId}`);
            console.log(`   Signaling URL: ${data.signalingUrl}`);
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

    // Optional: second allocation attempt to ensure availableSlots decrements
    try {
        console.log('\n4. Finding second Match (should succeed if slots remain)...');
        const response2 = await axios.post(`${BASE_URL}/api/match/find`, {
            region: 'us-east-1'
        });
        const data2 = response2.data;
        if (data2.found) {
            console.log('   Second match found, remaining slots should now decrease.');
        } else {
            console.log('   Second match not found (slots may be exhausted).');
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