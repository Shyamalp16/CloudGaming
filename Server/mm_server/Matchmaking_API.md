# Matchmaking API

The matchmaking API connects a user to one explicitly selected host. It does not expose a host directory, public addresses, machine details, or Redis identifiers.

Production requests must use HTTPS. The reverse proxy must terminate TLS, remove untrusted forwarding headers, and be one of the exact addresses in `TRUSTED_PROXY_IPS`. Request and response bodies are JSON. Unknown fields are rejected.

## Host heartbeat

`POST /api/host/heartbeat`

The host authenticates with its own credential. Never reuse one credential across multiple hosts.

```http
Authorization: Bearer <credential for this hostId>
Content-Type: application/json
```

```json
{
  "hostId": "319ca4b3-a64a-49f2-bfd9-c26e19908b5a",
  "roomId": "cf7f0241482d4742bb88952815b116b8",
  "pairingCode": "7b515173c8f3e4059cf6b7c694bf61a4",
  "region": "ca-central",
  "status": "idle",
  "capacity": 1,
  "availableSlots": 1
}
```

- `hostId` is a stable UUID and must have a matching entry in `HOST_CREDENTIALS_JSON`.
- `roomId` and `pairingCode` are independently generated 128-bit random hexadecimal values.
- The pairing code is single-use. After a successful exchange, Redis atomically marks it consumed and the next authenticated host heartbeat instructs the host to generate and publish a fresh code. Share the current code out of band only with the intended user.
- `capacity` is limited to 1–4. Omitted optional fields use safe defaults.
- A heartbeat expires automatically. No separate status endpoint exists.

Successful response:

```json
{ "success": true, "ttl": 30 }
```

## Find a paired host

`POST /api/match/find`

This endpoint is rate limited. It accepts only a pairing code; region-only discovery and host enumeration are intentionally unsupported.

```json
{ "pairingCode": "7b515173c8f3e4059cf6b7c694bf61a4" }
```

Successful response:

```json
{
  "found": true,
  "roomId": "cf7f0241482d4742bb88952815b116b8",
  "sessionId": "06488ba1-b82c-42eb-a7f2-8d9b9e477742",
  "pairingToken": "<short-lived signed token>",
  "expiresAt": 1786240000000,
  "iceServers": [
    {
      "urls": ["turns:turn.example.com:443?transport=tcp"],
      "username": "<ephemeral username>",
      "credential": "<ephemeral credential>"
    }
  ]
}
```

The client supplies the short-lived `pairingToken` through the WebSocket subprotocol during signaling. Do not place credentials or tokens in URLs, query strings, logs, analytics, or crash reports. The token is bound to `hostId`, `roomId`, and `sessionId`, and is rejected after expiry or replay.

An invalid, expired, unavailable, or already-consumed code returns the same non-enumerating not-found response.

## Production requirements

- Use `rediss://` with authentication and a dedicated Redis account/network boundary.
- Configure per-host credentials, a distinct pairing-token signing secret, protected metrics, exact HTTPS origins, exact trusted proxy IPs, and authenticated TURN.
- Store secrets in the deployment secret manager; never commit `.env` files.
- Do not log pairing codes, tokens, room/session/host identifiers, IP addresses, ICE candidates, authorization headers, or Redis connection details.
