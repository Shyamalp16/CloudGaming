const { randomUUID } = require('crypto');
const { WebSocket, WebSocketServer } = require('ws');
const { z } = require('zod');

const HostEvent = z
  .object({
    type: z.enum([
      'host.hello',
      'session.launch_ack',
      'session.game_ready',
      'session.stream_connected',
      'session.ended',
      'session.failed',
    ]),
    commandId: z.string().uuid().optional(),
    sessionId: z.string().uuid().optional(),
    payload: z.record(z.unknown()).default({}),
  })
  .strict();

class HostControlGateway {
  constructor({ authenticate, onEvent, onConnection, log }) {
    this.authenticate = authenticate;
    this.onEvent = onEvent;
    this.onConnection = onConnection;
    this.log = log;
    this.clients = new Map();
    this.wss = new WebSocketServer({ noServer: true, maxPayload: 16 * 1024 });
  }

  attach(server, path = '/api/v1/host/control') {
    server.on('upgrade', (request, socket, head) => {
      const url = new URL(request.url, 'http://localhost');
      if (url.pathname !== path) return;
      const hostId = url.searchParams.get('hostId') || '';
      if (!this.authenticate(request, hostId)) {
        socket.end('HTTP/1.1 401 Unauthorized\r\nConnection: close\r\n\r\n');
        return;
      }
      this.wss.handleUpgrade(request, socket, head, (ws) => this.accept(hostId.toLowerCase(), ws));
    });
  }

  accept(hostId, ws) {
    this.clients.get(hostId)?.close(1000, 'Replaced by reconnect');
    this.clients.set(hostId, ws);
    let events = Promise.resolve();
    this.onConnection?.(hostId, true);
    ws.on('message', (raw) => {
      let message;
      try {
        message = JSON.parse(raw.toString());
      } catch (_) {
        ws.close(1008, 'Invalid JSON');
        return;
      }
      const parsed = HostEvent.safeParse(message);
      if (parsed.success) {
        events = events
          .then(() => this.onEvent(hostId, parsed.data))
          .catch((error) => this.log('warn', 'Host control event failed', { hostId, error }));
      } else ws.close(1008, 'Invalid host event');
    });
    ws.on('close', () => {
      if (this.clients.get(hostId) === ws) {
        this.clients.delete(hostId);
        this.onConnection?.(hostId, false);
      }
    });
    ws.on('error', (error) => this.log('warn', 'Host control socket error', { hostId, error }));
    ws.send(JSON.stringify({ type: 'control.ready', hostId }));
  }

  connected(hostId) {
    return this.clients.get(hostId)?.readyState === WebSocket.OPEN;
  }

  connectedCount() {
    return this.clients.size;
  }

  send(hostId, type, sessionId, payload) {
    const ws = this.clients.get(hostId);
    if (!ws || ws.readyState !== WebSocket.OPEN || ws.bufferedAmount > 64 * 1024) return null;
    const commandId = randomUUID();
    ws.send(JSON.stringify({ type, commandId, sessionId, payload }));
    return commandId;
  }
}

module.exports = { HostControlGateway };
