# Matchmaking API Specification

## 1. Overview
This document defines the API for the automatic matchmaking service. The service connects Clients (Gamers) to available Hosts (Gaming PCs) by tracking host availability in Redis.

## 2. Data Model (Redis)

### Active Hosts
Hosts are stored as individual keys with a TTL (Time To Live). This ensures that if a Host crashes or loses internet, it automatically disappears from the available pool.

*   **Key:** `host:{hostId}`
*   **TTL:** 30 seconds (Refreshed by heartbeat)
*   **Structure (JSON):**

```json
{
  "hostId": "uuid-string-unique-per-session",
  "roomId": "room-id-for-signaling",
  "status": "idle",       // "idle" = ready for match, "busy" = in session
  "region": "us-east-1",  // or "local", "eu-central", etc.
  "lastHeartbeat": 1715620000000,
  "publicIp": "1.2.3.4",
  "gameInfo": {
    "name": "Desktop Session",
    "specs": "Generic Host"
  }
}
```

## 3. API Endpoints

### A. Host Endpoints (Used by C++ Host App)

#### 1. Register / Heartbeat
Host sends this every 20-25 seconds to register itself or keep its session alive.

*   **URL:** `POST /api/host/heartbeat`
*   **Headers:** `Content-Type: application/json`
*   **Body:**
    ```json
    {
      "hostId": "uuid-generated-by-host",
      "roomId": "room-generated-by-host",
      "region": "us-east-1",  // Optional, defaults to config
      "status": "idle"        // Optional, defaults to idle if new
    }
    ```
*   **Response (200 OK):**
    ```json
    {
      "success": true,
      "ttl": 30
    }
    ```

#### 2. Update Status
Host calls this when a peer connects via Signaling to mark itself as "busy".

*   **URL:** `POST /api/host/status`
*   **Body:**
    ```json
    {
      "hostId": "uuid-...",
      "status": "busy"
    }
    ```

### B. Client Endpoints (Used by Web/Electron Client)

#### 1. Find Match
Client calls this when the user clicks "Play".

*   **URL:** `POST /api/match/find`
*   **Body:**
    ```json
    {
      "region": "us-east-1" // Optional filter
    }
    ```
*   **Response (200 OK):**
    ```json
    {
      "found": true,
      "roomId": "room-id-from-host",
      "signalingUrl": "ws://play.yourdomain.com", // or wss://...
      "iceServers": [
        {
          "urls": "turn:turn.yourdomain.com:3478",
          "username": "timestamp:user",
          "credential": "generated-password"
        }
      ]
    }
    ```
*   **Response (404 Not Found):**
    ```json
    {
      "found": false,
      "message": "No available hosts in this region."
    }
    ```

## 4. Internal Logic

### Matchmaking Algorithm (Simple)
1.  Receive `POST /api/match/find`.
2.  Scan Redis for keys matching `host:*`.
3.  Filter hosts where `status === 'idle'`.
4.  (Optional) Filter by `region`.
5.  Select a random host (or first available).
6.  Mark that host as `allocated` (temporarily) in memory or Redis to prevent double-booking (race condition handling).
7.  Generate TURN credentials (using shared secret with Coturn).
8.  Return connection details to Client.

### Cleanup
*   Redis TTL automatically handles cleanup of dead hosts.
