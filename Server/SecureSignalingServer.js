const WebSocket = require('ws');
const url = require('url');

const server = new WebSocket.Server({ port: 3001 });

// Use a Map to store rooms. Key: roomId, Value: Set of connected clients (WebSockets)
const rooms = new Map();

server.on('connection', (ws, req) => {
    try {
        const parameters = new url.URL(req.url, `ws://${req.headers.host}`).searchParams;
        const roomId = parameters.get('roomId');

        if (!roomId) {
            console.log('Connection attempt without roomId. Rejecting.');
            ws.close(1008, 'RoomID is required');
            return;
        }

        // Get or create the room
        if (!rooms.has(roomId)) {
            console.log(`Creating new room: ${roomId}`);
            rooms.set(roomId, new Set());
        }

        const room = rooms.get(roomId);

        if (room.size >= 2) {
            console.log(`Room ${roomId} is full. Rejecting new connection.`);
            ws.close(1000, 'Room is full');
            return;
        }

        // Add the client to the room
        room.add(ws);
        // Store roomId on the ws object for easy access on close
        ws.roomId = roomId;

        console.log(`Client joined room ${roomId}. Total clients in room: ${room.size}`);

        ws.on('message', (message) => {
            let parsedMessage;
            try {
                // Try to parse the message as JSON. WebRTC signaling is typically JSON.
                parsedMessage = JSON.parse(message);
            } catch (error) {
                console.error(`Failed to parse message in room ${roomId}:`, error);
                // Don't forward malformed messages.
                return;
            }
            // The message is forwarded to the other peer in the same room
            forwardToOther(ws, room, parsedMessage);
        });

        ws.on('close', () => {
            const room = rooms.get(ws.roomId);
            if (room) {
                room.delete(ws); // Remove the disconnected client
                console.log(`Client left room ${ws.roomId}. Total clients in room: ${room.size}`);

                // Notify the remaining peer, if any
                if (room.size > 0) {
                    const otherPeer = [...room][0]; // The only one left
                    if (otherPeer && otherPeer.readyState === WebSocket.OPEN) {
                        try {
                            otherPeer.send(JSON.stringify({ type: 'peer-disconnected' }), (error) => {
                                if (error) {
                                    console.error('Error notifying peer of disconnection:', error);
                                }
                            });
                        } catch (error) {
                            console.error('Exception while notifying peer of disconnection:', error);
                        }
                    }
                }

                // If the room is now empty, delete it from the map
                if (room.size === 0) {
                    console.log(`Room ${ws.roomId} is empty. Deleting room.`);
                    rooms.delete(ws.roomId);
                }
            }
        });

        ws.on('error', (error) => {
            console.error(`WebSocket error in room ${ws.roomId || 'unknown'}:`, error);
        });

    } catch (error) {
        console.error('Error during connection setup:', error);
        ws.close(1011, 'Internal server error');
    }
});

function forwardToOther(sender, room, message) {
    // Find the other client in the room
    const otherPeer = [...room].find(client => client !== sender);

    if (otherPeer && otherPeer.readyState === WebSocket.OPEN) {
        try {
            // No need to stringify, as the original message is a Buffer or string
            otherPeer.send(message, (error) => {
                if (error) {
                    console.error('Error forwarding message to other peer:', error);
                }
            });
        } catch (error) {
            console.error('Exception while forwarding message:', error);
        }
    } else {
        // This can happen if one peer sends a message just as the other disconnects
        console.warn('No available peer in the room to forward the message to.');
    }
}

console.log('Secure Signaling Server listening on port 3001');

// Server-level error handling
server.on('error', (error) => {
    console.error('Server-level error:', error);
});
