# P2P Cloud Gaming / Remote Desktop

Low-latency Windows game and desktop streaming over WebRTC. The host captures a
target window and its audio, performs hardware H.264 encoding, and sends media
directly to a browser. Keyboard and mouse input return over dedicated WebRTC
DataChannels.

## Architecture

```text
Browser client
    |  HTTP(S): matchmaking and ICE configuration
    |  WebSocket: SDP and ICE signaling
    v
Matchmaker (:3000) <----> Redis <----> Signaling server (:3002)
                                      |
                                      v
Windows host (C++ / Go)
  HostRuntime -> target/session/profile/audio/input service ownership
  WGC capture -> D3D11 BGRA -> GPU BGRA-to-NV12 -> FFmpeg HW H.264
                                                    |
  WASAPI process loopback -> resample -> Opus       | WebRTC/SRTP
                                                    v
                                              Browser <video>

  Browser keyboard/buttons/wheel -> reliable ordered DataChannel --+
  Browser mouse movement          -> unordered lossy DataChannel ---+-> host input injection
```

| Component | Path | Stack |
|---|---|---|
| Host runtime | `Host/`, `gortc_main/` | C++, Go, Pion WebRTC |
| Signaling server | `Server/ScalableSignalingServer.js` | Node.js, ws, Redis |
| Matchmaker | `Server/mm_server/Matchmaker.js` | Node.js, Express, Redis |
| Browser client | `Client/html-server/index.html` | HTML, JavaScript, WebRTC |

## Windows host application and packaging

Launching `DisplayCaptureProject.exe` without arguments opens the notification-
area host application. It provides asynchronous start/stop controls, visible
game/process selection, a copyable pairing code, client/session state, and live
FPS, video/send bitrate, RTT, and audio state. The first launch generates a
versioned per-user config under `%LOCALAPPDATA%\CloudGamingHost`; credentials are
stored separately with Windows DPAPI and a user-only ACL.

Create the complete portable payload and validated WiX MSI with:

```powershell
.\packaging\Build-Package.ps1 -Version 0.1.0 -BuildInstaller
```

See `Installer/README.md` for signing, upgrade, firewall, uninstall, and signed
update-feed behavior.

## Pipeline behavior

### Capture, encode, and render

- Windows Graphics Capture runs from a free-threaded frame pool. Captured BGRA
  frames are copied into an application-owned D3D11 texture pool so the WGC
  callback can return immediately.
- Capture and transport queues are intentionally shallow and bounded. When the
  stream falls behind, stale frames are replaced by the newest frame instead of
  adding latency. Dropped frame duration is carried into the next RTP sample so
  the receiver timeline remains correct.
- Capture-size changes recreate the WGC frame pool and discard textures with the
  old dimensions. Capture callbacks are synchronized with shutdown before GPU
  resources are released.
- BGRA-to-NV12 conversion stays on the GPU through the D3D11 VideoProcessor and
  feeds FFmpeg hardware frames directly. NVENC, QSV, and AMF hardware encoders
  are supported without a CPU readback or intermediate muxing path.
- Encoder initialization retries are throttled to avoid a tight failure loop.
  RTCP feedback drives bitrate adaptation and rate-limited keyframe requests.
- Encoded packets enter a two-sample Go/Pion video queue. Reconnect generations
  prevent packets, callbacks, tracks, or statistics from an old peer connection
  from reaching a new session.
- The browser displays the remote `<video>` element directly. It does not copy
  every frame through a canvas. `requestVideoFrameCallback` is used only for
  measurement, while WebRTC receiver statistics provide displayed FPS, bitrate,
  loss, and RTT.
- Render callbacks, statistics polling, reconnect timers, media elements, and
  DataChannels are scoped to one connection generation and stopped on teardown.

### Audio

- WASAPI process-loopback capture can include the target process tree and emits
  48 kHz stereo Opus in 10 ms frames (480 RTP samples per packet).
- Audio startup waits for real device initialization and fails cleanly after a
  bounded timeout. Stop and restart paths release the active capture instance,
  audio client, resampler, and worker thread safely.
- Device formats are validated before conversion. Event-driven WASAPI and small
  device periods are preferred, with a configured fallback period. The
  resampler drains incomplete DMO output correctly and supports the low-cost
  linear path configured for streaming.
- Audio queues are shallow and bounded (three RTP packets in Go). A congested
  connection replaces stale audio instead of accumulating an audible backlog.
- Audio and video use a shared session reference for stable synchronization.
- RTCP loss reports can adapt Opus bitrate and toggle in-band FEC within the
  configured thresholds. Shutdown drains and returns queued packet buffers and
  terminates background workers.

### Keyboard and mouse input

- `keyPressChannel` is reliable and ordered. It carries keyboard transitions,
  mouse buttons, wheel input, and reset messages that must not be lost.
- `mouseChannel` is unordered with `maxRetransmits: 0`. It carries disposable
  high-frequency pointer movement so an old movement cannot delay a new one.
- Browser-side backpressure coalesces pending state: one latest transition per
  physical key/button, one latest mouse position, and accumulated wheel deltas.
  A `bufferedamountlow` handler resumes sending without polling or busy loops.
  Release and reset messages take priority so keys cannot remain stuck.
- The browser restores physically held keys and buttons after a DataChannel
  reconnect. Blur, disconnect, and session teardown send or locally apply a full
  input release.
- Go queues are bounded to 128 keyboard/control messages and 256 mouse messages.
  Overflow preserves safety releases while stale movement or redundant presses
  are discarded. Queue head indices avoid shifting the remaining slice on every
  event.
- The native injection queue is also bounded. On overload it clears stale work,
  releases held input, and resumes from a known state.
- Scan-code mapping handles keyboard layouts and extended keys. Duplicate or
  invalid transitions are ignored by the input state machine.
- Input is injected only while the target window is valid, visible, enabled, and
  foreground. Pointer coordinates are clamped to the target client area and
  converted to Windows virtual-desktop absolute coordinates.
- The injection worker can use the `Games` MMCSS class and time-critical Win32
  priority. Optional stuck-key recovery is disabled by default because long key
  holds are valid during gameplay.

### Connection and server reliability

- The native host has explicit `Stopped`, `Initializing`, `WaitingForTarget`,
  `Ready`, `Streaming`, `Reconnecting`, `Stopping`, and `Failed` states. One
  runtime controller owns startup, restart, target reattachment, and reverse-order cleanup.
- A separate session state machine owns pairing/authorization/connection state.
  Each match creates an expiring signed session token; stale or replayed session
  messages are rejected by the browser, server, host signaling, and input queue.
- Matchmaker HTTP responses are status-checked. Signaling reconnects with
  cancellable exponential backoff capped at 30 seconds plus bounded jitter and queues ICE candidates until the
  remote description exists.
- WebSocket, peer-connection, track, DataChannel, and statistics callbacks carry
  a connection generation. Late callbacks from a closed session are ignored.
- Replacing a Pion peer connection closes its channels, clears bounded media and
  input queues, and stops peer-specific RTT monitoring.
- The signaling server serializes messages per client, limits each pending chain
  to 64 messages, enforces message/rate/backpressure limits, and terminates
  clients that fail heartbeat checks.
- Signaling shutdown stops heartbeat work, drains clients, and closes Redis.
  Connection and room metrics use exactly-once disconnect bookkeeping.
- Redis Lua operations make host heartbeat/capacity updates and host claims
  atomic. Allocation reservations prevent concurrent clients from claiming the
  same capacity, while batched Redis reads reduce matchmaking round trips.
- `/readyz` reflects Redis readiness. Remote JWKS keys and TURN credential
  responses are cached; concurrent TURN requests are coalesced and bounded by
  response-size, status, and timeout checks.
- Browser diagnostic text is rendered as text, and never interpreted as HTML.

## Running locally or over LAN

The browser and host derive their endpoints from
`Client/html-server/network-config.json`. Change only `mode` when moving between
same-PC, LAN, and deployed testing:

| Mode | Open in the browser | Endpoint behavior |
|---|---|---|
| `local` | `http://localhost:8080` | Uses loopback for matchmaker and signaling |
| `lan` | `http://HOST_PC_IP:8080` | Uses the hostname/IP from the page URL |
| `production` | Deployed client URL | Uses the two URLs under `production` |

For a two-laptop test, set `"mode": "lan"`, start every service on the host
laptop, and open `http://<host-laptop-ip>:8080` on the client laptop. No source
URLs need to be edited. Allow inbound TCP ports 8080, 3000, and 3002 through
Windows Firewall on private networks.

Start the components in this order.

### 1. Redis

```powershell
redis-server
```

The default connection is `redis://127.0.0.1:6379`.

### 2. Signaling server

```powershell
cd Server
npm install
npm start
```

Use `npm run dev` for pretty development logs. The default signaling endpoint is
`ws://localhost:3002`.

### 3. Matchmaker

In a second terminal:

```powershell
cd Server
node mm_server/Matchmaker.js
```

The default matchmaker endpoint is `http://localhost:3000`.

For authenticated local/LAN testing, put a random value of at least 32
characters in `Server/.env` as `HOST_SECRET`, then set the same value in the
host process as `CLOUDGAMING_HOST_SECRET`. Secrets are never stored in
`config.json`.

### 4. Browser client

```powershell
cd Client/html-server
npx http-server . -p 8080 -c-1
```

Open the URL for the selected mode from the table above.

### 5. Windows host

Build `DisplayCaptureProject.sln` as **Release x64**, then run:

```powershell
$env:CLOUDGAMING_HOST_SECRET = '<same value as Server/.env HOST_SECRET>'
x64\Release\DisplayCaptureProject.exe
```

Keep `config.json` in the executable's working directory. Release x64 is strongly
recommended for capture and encode performance. The x64 post-build step copies
the required FFmpeg, Opus/OpenSSL, and Go/Pion runtime DLLs beside the executable
so it can be launched directly from `x64\Release` or `x64\Debug`.

## Configuration reference

### `Client/html-server/network-config.json`

| Key | Default | Description |
|---|---|---|
| `mode` | `local` | Endpoint switch: `local`, `lan`, or `production` |
| `ports.signaling` | `3002` | Signaling port for local and LAN modes |
| `ports.matchmaker` | `3000` | Matchmaker port for local and LAN modes |
| `production.signalingUrl` | deployed `wss://` URL | Public signaling endpoint |
| `production.matchmakerUrl` | deployed `https://` URL | Public matchmaker endpoint |

### `config.json`: host and window

| Key | Default | Description |
|---|---|---|
| `host.targetProcessName` | `vlc.exe` | Process whose window and audio are streamed |
| `host.matchmaker.heartbeatIntervalMs` | `20000` | Host registration refresh interval |
| `host.window.resizeClientArea` | `true` | Resize the target's client area at startup |
| `host.window.targetWidth` / `targetHeight` | `1920` / `1080` | Requested client-area dimensions |

### `host.video`

| Key | Default | Description |
|---|---|---|
| `fps` | `60` | Target capture and encode frame rate |
| `defaultProfile` | `1920x1080`, 60 FPS, 8 Mbps | Persistent operator default; valid resolutions are 720p/1080p/1440p |
| `allow120Fps` | `false` | Explicitly permit negotiated 120 FPS; otherwise only 30/60 are accepted |
| `bitrateStart` | `8000000` | Initial H.264 bitrate in bits per second |
| `bitrateMin` / `bitrateMax` | `8000000` / `12000000` | RTCP bitrate-controller limits |
| `hwFramePoolSize` | `3` | Reusable FFmpeg hardware-frame surfaces |
| `preset` | `p2` | Hardware-encoder speed preset |
| `rc` | `cbr` | Rate-control mode |
| `bf` / `rcLookahead` | `0` / `0` | Disable reordering and lookahead for low latency |
| `asyncDepth` / `surfaces` | `2` / `3` | Hardware encoder pipeline depth |
| `fullRange` | `false` | Encode limited-range YUV |
| `ignorePli` | `false` | Allow RTCP PLI to request keyframes |
| `minPliIntervalMs` | `500` | Minimum time between accepted PLI requests |
| `minPliLossThreshold` | `0.03` | Loss threshold used by PLI handling |
| `bitrateController.*` | see `config.json` | Increase/decrease cadence and clean-report threshold |
| `hdrToneMapping.*` | disabled | Optional Reinhard HDR-to-SDR controls |

### `host.capture`

| Key | Default | Description |
|---|---|---|
| `copyPoolSize` | `4` | Application-owned D3D11 capture textures |
| `maxQueueDepth` | `2` | Maximum frames waiting for encode |
| `framePoolBuffers` | `3` | WGC frame-pool buffer count |
| `cursor` | `false` | Include the host cursor in captured video |
| `borderRequired` | `false` | Request the WGC capture border |
| `mmcss.enable` / `priority` | `true` / `2` | Capture-thread MMCSS scheduling |

### `host.audio`

| Key | Default | Description |
|---|---|---|
| `processLoopback.enabled` | `true` | Capture audio from the target process |
| `processLoopback.includeProcessTree` | `true` | Include child-process audio |
| `processLoopback.fallbackToDeviceLoopback` | `false` | Explicit consent for device-wide audio fallback; never enabled implicitly |
| `bitrate` | `80000` | Initial Opus bitrate in bits per second |
| `complexity` / `expectedLossPerc` | `6` / `5` | Opus CPU/quality and expected-loss settings |
| `enableFec` / `enableDtx` | `true` / `false` | Opus FEC and discontinuous transmission |
| `frameSizeMs` / `channels` | `10` / `2` | Packet duration and stereo output |
| `wasapi.enforceEventDriven` | `true` | Require event-driven WASAPI capture |
| `wasapi.devicePeriodMs` / `fallbackPeriodMs` | `5` / `10` | Preferred and fallback device periods |
| `wasapi.force48kHzStereo` | `true` | Normalize input for WebRTC Opus |
| `wasapi.preferLinearResampling` | `true` | Prefer the low-overhead resampling path |
| `latency.enforceSingleFrameBuffering` | `true` | Keep only one 10 ms encoder frame buffered |
| `latency.targetOneWayLatencyMs` | `40` | Audio latency target used for diagnostics |
| `bitrateAdaptation.*` | enabled, `64k`-`128k` | RTCP-driven Opus bitrate and FEC policy |

### `host.input`

| Key | Default | Description |
|---|---|---|
| `releaseAllOnDisconnect` | `true` | Release held keys/buttons when a session closes |
| `stuckKeyTimeoutMs` | `2000` | Timeout used only when recovery is enabled |
| `enableStuckKeyRecovery` | `false` | Optional timeout recovery for stuck keys |
| `enableMouseSequencing` | `false` | Optional sequencing checks for mouse events |
| `enablePerEventLogging` | `false` | Verbose input-event logging |
| `enableAggregatedLogging` | `true` | Periodic aggregate input diagnostics |
| `maxPendingMessages` | `100` | Native input transport queue limit |
| `threadPriority.enableMMCSS` | `true` | Use MMCSS for the injection worker |
| `threadPriority.mmcssClass` | `Games` | MMCSS scheduling class |
| `threadPriority.enableTimeCritical` | `true` | Request time-critical Win32 priority |
| `adaptiveQualityControl.*` | enabled | RTT/loss/queue thresholds for disposable event dropping |

### Server environment

The server reads `Server/.env`. Important defaults are:

| Variable | Default | Description |
|---|---|---|
| `WS_PORT` / `MATCHMAKER_PORT` | `3002` / `3000` | Signaling and matchmaker ports |
| `REDIS_URL` | `redis://127.0.0.1:6379` | Shared Redis connection |
| `ROOM_CAPACITY` | `2` | Maximum clients assigned to a host room |
| `HOST_TTL_SECONDS` | `60` | Host heartbeat expiry |
| `ALLOCATION_RESERVATION_SECONDS` | `20` | Atomic matchmaking reservation lifetime |
| `HEARTBEAT_INTERVAL_MS` | `30000` | Signaling WebSocket heartbeat interval |
| `MESSAGE_MAX_BYTES` | `262144` | Maximum signaling message size |
| `BACKPRESSURE_CLOSE_THRESHOLD_BYTES` | `5242880` | Close clients exceeding this buffered amount |
| `HOST_SECRET` | unset | Expected host-registration secret; required and 32+ characters in production |
| `PAIRING_TOKEN_SECRET` | unset | HMAC key for short-lived pairing tokens; required and 32+ characters in production |
| `PAIRING_TOKEN_TTL_SECONDS` | `120` | Pairing token lifetime, 30–600 seconds |
| `ENABLE_SESSION_AUTH` | false outside production | Require role-bound signed sessions; always enabled in production |
| `ENABLE_AUTH` | unset/false | Enable JWT validation for client signaling |
| `REQUIRE_WSS` | unset/false | Reject non-secure WebSocket connections |
| `ALLOWED_ORIGINS` | unset | Optional comma-separated WebSocket origins |
| `METERED_DOMAIN` / `METERED_API_KEY` | unset | Optional Metered TURN credential source |

The server `HOST_SECRET` must match the host process environment variable
`CLOUDGAMING_HOST_SECRET`. For deployment, use unique random values and never
put them in source-controlled JSON.

## Stream profiles

`StreamProfileManager` is the only native authority for requested and active
profiles. The browser advertises H.264 receive limits over the authenticated
signaling session, then requests one of 1280x720, 1920x1080, or 2560x1440 at
30/60 FPS (120 only when explicitly enabled). Unsupported requests are rejected
with a reason and the active profile remains unchanged. The capture consumer
applies accepted changes between frames, refreshes IDR/header state, and keeps
RTP timing monotonic. Resolution changes rebuild encoder surfaces; bitrate
changes use the encoder's safe reconfiguration boundary.

## Production deployment

Production intentionally fails closed. Complete every item before changing
`network-config.json` to `production`:

1. Deploy Redis privately and set `REDIS_URL` on both Node services.
2. Terminate TLS at the service or trusted proxy. Set the browser endpoints to
   public `https://` matchmaker and `wss://` signaling URLs.
3. Set `NODE_ENV=production`, `REQUIRE_WSS=true`, `ENABLE_SESSION_AUTH=true`, a
   32+ character `HOST_SECRET`, and a different 32+ character
   `PAIRING_TOKEN_SECRET` on the server.
4. Configure `ALLOWED_ORIGINS` with the exact deployed browser origin. If JWT
   client authentication is integrated, pass it separately as `accessToken`;
   the `token` query parameter is reserved for the rotating pairing credential.
5. Configure TURN. Metered can be used through `METERED_DOMAIN` and
   `METERED_API_KEY`, or provide the host with `PION_TURN_URLS`,
   `PION_TURN_USERNAME`, and `PION_TURN_CREDENTIAL`. The native host refuses to
   start in production without complete TURN credentials.
6. On the host, set `CLOUDGAMING_HOST_SECRET` to the server host secret and run
   the Release x64 binary. Do not pass secrets in URLs or command-line arguments.

Example host environment (use a secret manager in real deployment):

```powershell
$env:CLOUDGAMING_HOST_SECRET = '<32+ random characters>'
$env:PION_TURN_URLS = 'turns:turn.example.com:5349?transport=tcp,turn:turn.example.com:3478?transport=udp'
$env:PION_TURN_USERNAME = '<ephemeral username>'
$env:PION_TURN_CREDENTIAL = '<ephemeral credential>'
.\x64\Release\DisplayCaptureProject.exe
```

## Operations and diagnostics

- Node exposes `/healthz`, `/readyz`, and Prometheus `/metrics`; readiness is
  false while Redis is unavailable.
- The host writes redacted structured JSONL logs to
  `%LOCALAPPDATA%\CloudGamingHost\logs`, rotating at 5 MiB and retaining five
  files. The directory and files receive a protected DACL for the current user
  and SYSTEM.
- Unhandled native crashes write minidumps under
  `%LOCALAPPDATA%\CloudGamingHost\dumps`.
- The runtime health snapshot aggregates lifecycle/session/profile state, peer
  state, audio state/failure reason, input/capture queue depths and drops,
  encoder state/bitrate, and logging failures every 30 seconds.
- Generate a sanitized offline support bundle with:

```powershell
.\x64\Release\DisplayCaptureProject.exe --support-bundle
```

The command prints the protected output directory under
`%LOCALAPPDATA%\CloudGamingHost\support`. It includes sanitized configuration,
health metadata, and the current structured log. Secrets, tokens, passwords,
credentials, and authorization values are redacted.

## Verification and troubleshooting

Run the focused native safety tests after every host change:

```powershell
.\x64\Release\DisplayCaptureProject.exe --self-test
```

Run server and Go checks with:

```powershell
cd Server
npm.cmd test -- --runInBand
npm.cmd run lint

cd ..\gortc_main
$env:CGO_ENABLED = '1'
$env:Path = 'C:\msys64\mingw64\bin;' + $env:Path
go test ./...
```

Common failures:

- `WaitingForTarget`: verify `host.targetProcessName`, start the process, and
  ensure it has a visible top-level window.
- Production startup failure: verify WSS/HTTPS endpoints, all three TURN host
  variables, and a 32+ character host secret.
- No audio: inspect the audio failure reason in the health log. Device-wide
  fallback occurs only when `fallbackToDeviceLoopback` is explicitly true.
- Pairing rejected: ensure both server secrets are present, clocks are correct,
  and the browser requested a fresh match instead of replaying an expired token.
- Stuck input after loss: confirm input reset records appear. Queue overflow,
  DataChannel close, signaling loss, peer replacement, and shutdown all force a
  full release before new input is accepted.
