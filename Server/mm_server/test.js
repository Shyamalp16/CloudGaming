const axios = require('axios'); // Ensure axios is installed: npm install axios

const BASE_URL = 'http://localhost:3000/api'; // Adjust port if needed

async function runTest() {
    console.log("--- Starting Matchmaking Flow Test ---");

    // 1. Host sends Heartbeat (Registers as IDLE)
    console.log("\n1. Host sending heartbeat...");
    try {
        await axios.post(`${BASE_URL}/host/heartbeat`, {
            hostId: 'test-host-01',
            roomId: 'test-room-01',
            region: 'us-east',
            status: 'idle'
        });
        console.log("✅ Host registered successfully.");
    } catch (error) {
        console.error("❌ Host heartbeat failed:", error.message);
        return;
    }

    console.log("⏳ Waiting 15 seconds...");
    await new Promise(resolve => setTimeout(resolve, 1000));
    console.log("▶️ Resuming...");

    // 2. Client looks for a match
    console.log("\n2. Client requesting match...");
    try {
        const response = await axios.post(`${BASE_URL}/match/find`, {
            region: 'us-east'
        });
        console.log("✅ Match found!", response.data);
        
        if (response.data.roomId !== 'test-room-01') {
             console.warn("⚠️ Warning: Matched with unexpected room:", response.data.roomId);
        }
    } catch (error) {
        console.error("❌ Match request failed:", error.response ? error.response.data : error.message);
    }

    // 3. Second Client looks for a match (Should FAIL)
    console.log("\n3. Second Client requesting match (expecting failure)...");
    try {
        await axios.post(`${BASE_URL}/match/find`, {
            region: 'us-east'
        });
        console.log("❌ Error: Second client found a match! (Should have been busy)");
    } catch (error) {
        if (error.response && error.response.status === 404) {
            console.log("✅ Correctly rejected: No hosts available (Host was allocated).");
        } else {
            console.error("❌ Unexpected error:", error.message);
        }
    }
}

runTest();