# P2P Cloud Gaming / Remote Desktop

Low-latency Windows game and desktop streaming over WebRTC. The host captures a
target window and its audio, performs hardware H.264 encoding, and sends media
directly to a browser. Keyboard and mouse input return over dedicated WebRTC
DataChannels.

## Architecture

```text
Browser client
    |  HTTP(S): game catalog, timed sessions, and ICE configuration
    |  WebSocket: SDP and ICE signaling
    v
Matchmaker (:3000) <----> Redis <----> Signaling server (:3002)
                                      |
                                      v
Windows host (C++ / Go)
  Steam/manual inventory -> persistent control channel -> unattended launcher
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

## Unattended marketplace flow

The current desktop clients are `../ReflexDesktop` for hosts and `../ReflexClient`
for players. A host selects installed Steam games or adds a manual executable,
starts hosting once, and can close the Electron window while the native agent
continues in the background. No running-process selection or join code is part
of the marketplace path.

The player catalog lists only games with a live, idle control-connected host.
Pressing Play creates a timed session; the matchmaker ranks compatible hosts by
regional RTT, atomically leases one, and sends an idempotent prepare command.
The native host launches the game, discovers its window, starts process audio
and capture, and pre-authorizes the session. The player receives a short-lived
room bootstrap only after the game is ready. Session duration starts when the
stream connects. Expiry, player cancellation, game exit, launch timeout, or a
host disconnect all stop and clean the session before the host returns to idle.

Preparation failures are reassigned to the next compatible host. Active host
disconnects have a reconnect grace period and then fail visibly rather than
leaving a session stuck. Redis provides atomic leases and live orchestration;
`Server/db/schema.sql` defines the durable Postgres/Supabase record model.

## Windows host application and packaging

`ReflexDesktop` is the normal host control surface. It provides background
start/stop, Steam discovery, manual-game registration, offering toggles, and
current session state. `DisplayCaptureProject.exe` remains the native agent and
notification-area diagnostic application; its process-selection and pairing
controls are retained only for the legacy private-session path. The first
launch generates a versioned per-user config under
`%LOCALAPPDATA%\CloudGamingHost`; credentials and device identity are stored
separately with Windows DPAPI and a user-only ACL.

Create an unsigned, non-distributable development payload with:

```powershell
.\packaging\Build-Package.ps1 -Version 0.1.0 -Configuration Debug -AllowUnsignedDevelopment
```

The script refuses to create an unsigned MSI. See `Installer/README.md` for the
signed installer, upgrade, firewall, uninstall, and update-feed procedure.

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

## Running locally or remotely

After generating the protected local environment once, start the complete local
stack with visible service logs:

```powershell
.\packaging\Start-LocalDevelopment.ps1
```

Windows users can alternatively double-click `Start-CloudGaming.bat`, which is
a thin wrapper around the same validated PowerShell launcher.

The launcher uses the Redis installation in the default WSL distribution,
starts signaling (including health/readiness) on 3002, matchmaker on 3000, and
the browser server on 8080, then opens the tray host and browser. Close the service
console windows to stop the stack. Use `-NoHost` or `-NoBrowser` when those
components are already running.

The browser and host derive their endpoints from
`Client/html-server/network-config.json`. Cleartext LAN access is deliberately
disabled because it would expose pairing and remote input traffic to the local
network:

| Mode | Open in the browser | Endpoint behavior |
|---|---|---|
| `local` | `http://localhost:8080` | Uses loopback for matchmaker and signaling |
| `production` | Deployed client URL | Uses the two URLs under `production` |

For a two-laptop test, use `production` mode behind an HTTPS/WSS reverse proxy
with a certificate trusted by both machines. Expose only the proxy; keep Node,
Redis, and the Windows host ports on loopback or a private service network. The
installer does not create an inbound firewall rule.

Start the components in this order.

### 1. Redis

```powershell
redis-server
```

The default connection is `redis://127.0.0.1:6379`.

### 2. Signaling server

```powershell
cd Server
npm ci
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

After building the host, generate a per-host server credential and import the
host copy into DPAPI. The transfer file is deleted after a successful import:

```powershell
$hostExe = (Resolve-Path .\x64\Release\DisplayCaptureProject.exe).Path
$deviceOutput = @(& $hostExe --device-id 2>&1)
$hostId = [regex]::Match(($deviceOutput -join "`n"),
  '(?im)^\s*([0-9a-f]{8}-[0-9a-f]{4}-[1-5][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12})\s*$').Groups[1].Value
if (-not $hostId) { throw 'Could not load the host device identity.' }
.\packaging\New-ServerEnvironment.ps1 -OutputPath .\Server\.env `
  -HostId $hostId -Environment development -AllowedOrigins http://localhost:8080
.\packaging\Configure-Host.ps1 -HostExecutable $hostExe `
  -HostCredentialFile .\Server\.env.host-credential.json
```

Do not reuse that credential for another host. Secrets are never stored in
`config.json`.

### 4. Browser client

```powershell
cd Client/html-server
npx http-server . -p 8080 -c-1
```

Open the URL for the selected mode from the table above.

### 5. Windows host

Build `DisplayCaptureProject.sln` as **Release x64**, complete the credential
setup above, then run:

```powershell
x64\Release\DisplayCaptureProject.exe
```

Keep `config.json` in the executable's working directory. Release x64 is strongly
recommended for capture and encode performance. The x64 post-build step copies
the required FFmpeg, Opus/OpenSSL, and Go/Pion runtime DLLs beside the executable
so it can be launched directly from `x64\Release` or `x64\Debug`.

Distributable builds require Go 1.26.5 for `gortc_main` and approved OpenSSL
DLLs whose exact hashes are recorded in `packaging/dependency-lock.json`. The
packaging script fails closed if the Go runtime is older, a dependency hash is
blank or different, the source tree is dirty, or release signatures are absent.
See `Installer/README.md` for the signed release procedure.

## Configuration reference

### `Client/html-server/network-config.json`

| Key | Default | Description |
|---|---|---|
| `mode` | `local` | Endpoint switch: `local` or `production` |
| `ports.signaling` | `3002` | Loopback signaling port in local mode |
| `ports.matchmaker` | `3000` | Loopback matchmaker port in local mode |
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
| `borderRequired` | `true` | Keep the Windows capture indicator visible while streaming |
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
| `HEALTH_PORT` | `8081` | Loopback health/readiness/metrics endpoint; kept separate from the browser development server on 8080 |
| `REDIS_URL` | `redis://127.0.0.1:6379` | Shared Redis connection |
| `ROOM_CAPACITY` | `2` | Maximum clients assigned to a host room |
| `HOST_TTL_SECONDS` | `60` | Host heartbeat expiry |
| `ALLOCATION_RESERVATION_SECONDS` | `20` | Atomic matchmaking reservation lifetime |
| `HEARTBEAT_INTERVAL_MS` | `30000` | Signaling WebSocket heartbeat interval |
| `MESSAGE_MAX_BYTES` | `262144` | Maximum signaling message size |
| `BACKPRESSURE_CLOSE_THRESHOLD_BYTES` | `5242880` | Close clients exceeding this buffered amount |
| `HOST_CREDENTIALS_JSON` | unset | JSON map of canonical host UUIDs to unique 32+ character credentials; required in production |
| `PAIRING_TOKEN_SECRET` | unset | HMAC key for short-lived pairing tokens; required and 32+ characters in production |
| `PAIRING_TOKEN_TTL_SECONDS` | `120` | Pairing token lifetime, 30–600 seconds |
| `ENABLE_SESSION_AUTH` | true | Require role-, host-, room-, and session-bound signed pairing tokens; always enabled in production |
| `ENABLE_AUTH` | unset/false | Enable JWT validation for client signaling |
| `REQUIRE_WSS` | unset/false | Reject non-secure WebSocket connections |
| `ALLOWED_ORIGINS` | unset | Exact comma-separated browser origins; required in production |
| `TRUSTED_PROXY_IPS` | unset | Exact TLS reverse-proxy addresses; required in production |
| `METRICS_SECRET` | unset | Distinct bearer credential protecting `/metrics`; required in production |
| `METERED_DOMAIN` / `METERED_API_KEY` | unset | Authenticated TURN credential source; required in production |

Each entry in `HOST_CREDENTIALS_JSON` must match the DPAPI-protected credential
on exactly one host. Generate these files with `New-ServerEnvironment.ps1`, keep
the server file in a secret manager, and delete every plaintext transfer file
after importing it with `Configure-Host.ps1`.

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
3. Generate the production environment with `New-ServerEnvironment.ps1`. It
   creates per-host credentials, distinct pairing/metrics secrets, strict WSS,
   and protected file ACLs. Store the result in the deployment secret manager.
4. Configure `ALLOWED_ORIGINS` with exact HTTPS browser origins and
   `TRUSTED_PROXY_IPS` with exact proxy addresses. Signaling metadata and tokens
   travel in the redacted WebSocket protocol header, never in a URL.
5. Configure Metered TURN with `METERED_DOMAIN` and `METERED_API_KEY`, and
   provide the host with `PION_TURN_URLS`,
   `PION_TURN_USERNAME`, and `PION_TURN_CREDENTIAL`. The native host refuses to
   start in production without a real TURN URL and bounded credentials.
6. Import the matching host credential with `Configure-Host.ps1`, which protects
   it with DPAPI and deletes the plaintext transfer file. Do not pass secrets in
   URLs or command-line arguments.

Example host environment (use a secret manager in real deployment):

```powershell
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
