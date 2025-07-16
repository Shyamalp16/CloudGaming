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

// In-memory map for clients connected to THIS server instance.
// Map<roomId, Set<WebSocket>>
const localRooms = new Map();

// Shared handler for all messages from Redis Pub/Sub.
function handleRedisMessage(message, channel) {
    try {
        // Extract roomId from the channel name (e.g., 'room:123' -> '123')
        const roomId = channel.replace(/^room:/, '');
        const { senderId, data } = JSON.parse(message);

        const clientsInRoom = localRooms.get(roomId);
        if (clientsInRoom) {
            clientsInRoom.forEach(client => {
                // Forward the message to all clients in the room on this instance,
                // except for the original sender.
                if (client.clientId !== senderId && client.readyState === WebSocket.OPEN) {
                    client.send(JSON.stringify(data));
                }
            });
        }
    } catch (error) {
        console.error(`Error handling Redis message on channel ${channel}:`, error);
    }
}

async function main() {
    await redisClient.connect();
    await subscriber.connect();
    console.log('Connected to Redis.');

    // Subscribe to all room channels using a single pattern subscription.
    // This is highly scalable as we don't create a new subscription for each client or room.
    await subscriber.pSubscribe('room:*', handleRedisMessage);
    console.log('Subscribed to Redis channel pattern room:*');

    server.on('connection', async (ws, request) => {
        try {
            if (!request || !request.url || !request.headers || !request.headers.host) {
                console.error('Malformed connection request: Missing request object, URL, or Host header.', { requestUrl: request?.url, requestHeaders: request?.headers });
                ws.close(1008, 'Malformed request');
                return;
            }
            const parameters = new url.URL(request.url, `ws://${request.headers.host}`).searchParams;
            const roomId = parameters.get('roomId');

            if (!roomId) {
                console.log('Connection attempt without roomId. Rejecting.');
                ws.close(1008, 'RoomID is required');
                return;
            }

            const roomKey = `room:${roomId}`;
            const roomSize = await redisClient.sCard(roomKey);

            if (roomSize >= 2) {
                console.log(`Room ${roomId} is full. Rejecting new connection.`);
                ws.close(1000, 'Room is full');
                return;
            }
            
            if (roomSize === 0) {
                await redisClient.persist(roomKey);
                console.log(`Room ${roomId} was empty, removed TTL.`);
            }

            const clientId = `client:${Date.now()}:${Math.random()}`;
            await redisClient.sAdd(roomKey, clientId);

            // Store metadata on the WebSocket object for easy access
            ws.roomId = roomId;
            ws.clientId = clientId;

            // Add the client to the in-memory map for this server instance.
            if (!localRooms.has(roomId)) {
                localRooms.set(roomId, new Set());
            }
            localRooms.get(roomId).add(ws);

            console.log(`Client ${clientId} joined room ${roomId}. This instance now has ${localRooms.get(roomId).size} client(s) in this room.`);

            ws.on('message', async (message) => {
                try {
                    const parsedMessage = JSON.parse(message);
                    // Publish the message to the Redis channel for this room.
                    // All subscribed server instances will receive it.
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
                
                // Remove the client from the global room set in Redis.
                await redisClient.sRem(roomKey, ws.clientId);

                // Notify the other peer about the disconnection by publishing to the room channel.
                await redisClient.publish(roomKey, JSON.stringify({
                    senderId: ws.clientId,
                    data: { type: 'peer-disconnected' }
                }));

                // Remove the client from this instance's in-memory map.
                const roomClients = localRooms.get(ws.roomId);
                if (roomClients) {
                    roomClients.delete(ws);
                    if (roomClients.size === 0) {
                        localRooms.delete(ws.roomId);
                        console.log(`Room ${ws.roomId} is now empty on this instance.`);
                    }
                }

                // If the room is now empty globally, set it to expire.
                const remainingClients = await redisClient.sCard(roomKey);
                if (remainingClients === 0) {
                    console.log(`Room ${roomId} is now empty globally. Setting TTL.`);
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
