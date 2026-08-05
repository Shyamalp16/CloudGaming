# P2P Cloud Gaming / Remote Desktop

Low-latency cloud gaming and remote desktop over WebRTC. A Windows host captures screen + audio, encodes via FFmpeg H.264, and streams peer-to-peer to a browser client. Input is forwarded back over WebRTC DataChannels.

---

## Architecture

```
Browser (Client)
    │  WebSocket (SDP/ICE)
    ▼
Signaling Server (Node.js :3002)  ◄──► Redis Pub/Sub (multi-node)
    │
    ▼
Host (C++ / Go)
  ├─ WGC Capture  →  D3D11 GPU copy  →  FFmpeg NVENC/QSV/AMF H.264
  ├─ WASAPI Audio  →  Opus encode
  └─ Pion WebRTC  ──── SRTP video+audio ──►  Client
                   ◄─── DataChannels (input, ping/pong) ──  Client

Matchmaker (Node.js :3000)  — host registration & session assignment
```

**Components:**

| Component | Path | Stack |
|---|---|---|
| Host runtime | `Host/`, `gortc_main/` | C++, Go (Pion WebRTC) |
| Signaling server | `Server/ScalableSignalingServer.js` | Node.js, ws, Redis |
| Matchmaker | `Server/mm_server/Matchmaker.js` | Node.js, Express |
| Client | `Client/html-server/index.html` | HTML/JS, WebRTC API |

---

## Running

Start components in this order:

### 1. Client (static file server)

```bash
cd Client/html-server
npx http-server . -p 8080 -c-1
# open http://localhost:8080
```

The host and browser share `Client/html-server/network-config.json`. Change only
its `mode` field when switching environments:

| Mode | Browser URL | Signaling and matchmaker |
|---|---|---|
| `local` | `http://localhost:8080` | Loopback on the same PC |
| `lan` | `http://HOST_PC_IP:8080` | Automatically uses `HOST_PC_IP` from the page URL |
| `production` | Your deployed client URL | Uses the public endpoints in `network-config.json` |

For a two-laptop LAN test, set `"mode": "lan"`, run the client server on the
hosting laptop, and open `http://<hosting-laptop-ip>:8080` on the other laptop.
No source URLs need to be changed. Windows Firewall must allow TCP ports 8080,
3000, and 3002 on private networks.

### 2. Redis

```bash
redis-server
# default port 6379
```

### 3. Signaling Server

```bash
cd Server
npm install
npm start          # production  (ws://localhost:3002)
# or: npm run dev  # pretty logs
```

### 4. Matchmaker Server

```bash
cd Server
node mm_server/Matchmaker.js
# listens on http://localhost:3000
```

### 5. Host (C++ runtime)

Build the Visual Studio solution (`DisplayCaptureProject.sln`, **Release x64**) for streaming performance, then run:

```
x64\Release\DisplayCaptureProject.exe
```

`config.json` must be in the working directory.

---

## Configuration Reference

### `Client/html-server/network-config.json`

| Key | Default | Description |
|---|---|---|
| `mode` | `local` | One switch: `local`, `lan`, or `production` |
| `ports.signaling` | `3002` | Local/LAN signaling port |
| `ports.matchmaker` | `3000` | Local/LAN matchmaker port |
| `production.signalingUrl` | — | Public `wss://` signaling endpoint |
| `production.matchmakerUrl` | — | Public `https://` matchmaker endpoint |

### `config.json`

### `host.video`

| Key | Default | Description |
|---|---|---|
| `fps` | `120` | Target capture/encode FPS |
| `bitrateStart` | `40000000` | Initial encode bitrate (bps) |
| `bitrateMin` / `bitrateMax` | `20M` / `80M` | ABR clamp range |
| `preset` | `p2` | NVENC preset (`p1`–`p7`, lower = faster) |
| `rc` | `cbr` | Rate control (`cbr`, `vbr`) |
| `bf` | `0` | B-frames (0 = lowest latency) |
| `fullRange` | `false` | YUV color range (false = limited/TV) |
| `hdrToneMapping.enabled` | `false` | SDR tone-map HDR signal before encode |

### `host.capture`

| Key | Default | Description |
|---|---|---|
| `copyPoolSize` | `4` | Application-owned D3D11 texture pool depth |
| `maxQueueDepth` | `2` | Max frames queued for encode |
| `framePoolBuffers` | `3` | WGC frame-pool buffers |
| `cursor` | `false` | Render cursor into capture |
| `mmcss.enable` | `true` | Boost capture thread via MMCSS |

### `host.audio`

| Key | Default | Description |
|---|---|---|
| `processLoopback.enabled` | `false` | Capture only target process audio (requires Win11) |
| `bitrate` | `80000` | Opus bitrate (bps) |
| `channels` | `2` | Output channels |
| `frameSizeMs` | `10` | Opus frame size |
| `enableFec` | `true` | Forward error correction |
| `wasapi.preferExclusiveMode` | `false` | Exclusive WASAPI mode (lower latency, may conflict) |

### `host.input`

| Key | Default | Description |
|---|---|---|
| `injectionPolicy` | `REQUIRE_FOREGROUND` | Only inject when target window is foreground |
| `blockWinKeys` | `true` | Block Win/system keys on host during session |
| `mousePolicy` | `DPI_AWARE` | Mouse coordinate scaling policy |
| `clipCursorToTarget` | `false` | Confine cursor to target window |
| `maxInjectHz` | `10000` | Max input injection rate |
| `usePionDataChannels` | `true` | Route input over WebRTC DataChannels |

### `host.matchmaker`

| Key | Default | Description |
|---|---|---|
| `hostSecret` | `HELLO-MFS` | Shared secret for host registration |
| `heartbeatIntervalMs` | `25000` | Host keepalive interval |
