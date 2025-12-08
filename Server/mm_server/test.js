const axios = require('axios'); // You might need to install axios: npm install axios

const HOST_URL = 'http://localhost:3000/api/host/heartbeat'; // Check your port

async function sendHeartbeat() {
    try {
        const response = await axios.post(HOST_URL, {
            hostId: 'host-simulation-1',
            roomId: 'room-simulation-A',
            region: 'eu-central',
            status: 'ready'
        });
        console.log('Heartbeat sent:', response.data);
    } catch (error) {
        console.error('Error sending heartbeat:', error.message);
    }
}

// Send a heartbeat every 5 seconds
setInterval(sendHeartbeat, 5000);
console.log('Starting heartbeat simulation...');