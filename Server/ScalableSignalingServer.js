const WebSocket = require('ws');
const url = require('url');
const { createClient } = require('redis');

// --- Configuration ---
const WEBSOCKET_PORT = 3002;
const REDIS_URL = 'redis://127.0.0.1:6379'; // Default local Redis URL

// --- Redis Clients ---
// We need two clients because a client in subscriber mode cannot issue other commands.
const redisClient = createClient({ url: REDIS_URL });
const subscriber = redisClient.duplicate();

const server = new WebSocket.Server({ port: WEBSOCKET_PORT });

async function main() {
    await redisClient.connect();
    await subscriber.connect();
    console.log('Connected to Redis.');

    server.on('connection', async (ws, request) => {
        try {
            if (!request || !request.url || !request.headers || !request.headers.host) {
                console.error('Malformed connection request: Missing request object, URL, or Host header.', { requestUrl: request?.url, requestHeaders: request?.headers });
                ws.close(1008, 'Malformed request');
                return;
            }
            console.log(`[WebSocket Server] Received connection request URL: ${request.url}`);
            const parameters = new url.URL(request.url, `ws://${request.headers.host}`).searchParams;
            const roomId = parameters.get('roomId');

            if (!roomId) {
                console.log('Connection attempt without roomId. Rejecting.');
                ws.close(1008, 'RoomID is required');
                return;
            }

            // Use a Redis Set to manage clients in a room.
            // SADD returns the number of elements added (1 if new, 0 if exists).
            // SCARD gives the total size of the set.
            const roomKey = `room:${roomId}`;
            const roomSize = await redisClient.sCard(roomKey);

            // If a client joins a room that was previously empty and had a TTL set, persist the key
            if (roomSize === 0) {
                await redisClient.persist(roomKey);
                console.log(`Room ${roomId} was empty, removed TTL.`);
            }

            if (roomSize >= 2) {
                console.log(`Room ${roomId} is full. Rejecting new connection.`);
                ws.close(1000, 'Room is full');
                return;
            }

            // Store a unique ID for this client in the room's set
            const clientId = `client:${Date.now()}:${Math.random()}`;
            await redisClient.sAdd(roomKey, clientId);

            // Store metadata on the WebSocket object for easy access
            ws.roomId = roomId;
            ws.clientId = clientId;

            console.log(`Client ${clientId} joined room ${roomId}.`);

            // --- Subscribe to the Redis channel for this room ---
            // The channel name is the same as the room key
            await subscriber.subscribe(roomKey, (message) => {
                // This callback receives messages published to the channel.
                const { senderId, data } = JSON.parse(message);

                // Don't send the message back to the original sender
                if (ws.clientId !== senderId && ws.readyState === WebSocket.OPEN) {
                    ws.send(JSON.stringify(data));
                }
            });

            ws.on('message', async (message) => {
                try {
                    const parsedMessage = JSON.parse(message);
                    // Publish the message to the Redis channel for this room.
                    // All subscribed servers (including this one) will receive it.
                    await redisClient.publish(roomKey, JSON.stringify({
                        senderId: ws.clientId,
                        data: parsedMessage
                    }));
                } catch (error) {
                    console.error(`Error processing message from ${ws.clientId} in room ${ws.roomId}:`, error);
                }
            });

            ws.on('close', async () => {
                console.log(`Client ${ws.clientId} disconnected from room ${ws.roomId}.`);
                // Remove the client from the Redis set
                await redisClient.sRem(roomKey, ws.clientId);
                
                // Unsubscribe from the channel to prevent memory leaks
                await subscriber.unsubscribe(roomKey);

                const remainingClients = await redisClient.sCard(roomKey);
                if (remainingClients === 0) {
                    console.log(`Room ${roomId} is now empty. Setting TTL.`);
                    await redisClient.expire(roomKey, 120); // Set TTL to 120 seconds
                }
            });

            ws.on('error', (error) => {
                console.error(`WebSocket error for client ${ws.clientId} in room ${ws.roomId}:`, error);
            });

        } catch (error) {
            console.error('Error during connection setup:', error);
            ws.close(1011, 'Internal server error');
        }
    });

    console.log(`Scalable Signaling Server listening on port ${WEBSOCKET_PORT}`);
}

main().catch(err => {
    console.error('Failed to start server:', err);
    process.exit(1);
});

// Graceful shutdown
process.on('SIGINT', async () => {
    console.log('Shutting down gracefully...');
    await redisClient.quit();
    await subscriber.quit();
    server.close();
    process.exit(0);
});
