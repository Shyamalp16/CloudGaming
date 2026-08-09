# Security architecture

## Security objectives

The host exposes screen/audio output and remote keyboard/mouse control, so compromise is equivalent to interactive access to the signed-in Windows session. The design therefore defaults to denial: a client must know a fresh pairing code, obtain a short-lived server-signed session token, establish authenticated WebRTC, and receive local user approval before input is enabled.

## Trust boundaries

1. **Windows host:** the tray application, per-user DPAPI credential, signed runtime DLLs, configuration, and updater. Pairing and input approval happen here.
2. **Public edge:** TLS termination and an exact allowlist of trusted proxy IPs. Forwarded TLS headers are ignored from every other address.
3. **Matchmaker/signaling:** authenticated host heartbeats, one-time pairing exchange, short-lived role-bound session tokens, strict origins/subprotocols, bounded messages, and fail-closed distributed rate limits.
4. **Redis:** private authenticated `rediss://` storage used for leases, reservations, replay prevention, and rate-limit state. It must not be internet-accessible.
5. **Browser client:** a static page with a restrictive CSP. Secrets are sent in authorization/subprotocol headers, not URLs, and are kept only for the active session.
6. **Release system:** isolated Windows signing runner, native dependency hash lock, SBOM, signed MSI, signed update manifest, and compiled update trust roots.

## Required production controls

- Use HTTPS/WSS only. Plain HTTP/WS is permitted solely on loopback for local development.
- Use a separate random credential of at least 32 bytes per host. Store the server-side mapping in a secret manager and the host copy with DPAPI.
- Require session authentication, exact browser origins, an exact WebSocket subprotocol, and an unpredictable pairing-token key of at least 32 bytes.
- Put Redis on a private network with TLS, username/password authentication, backups, and key eviction disabled for active lease data.
- Bind Node services to a private interface. Expose them only through a patched reverse proxy with request-size and connection limits.
- Protect metrics with a distinct bearer secret and keep health probes free of sensitive data.
- Run the Windows host as the signed-in standard user, not as Administrator or LocalSystem. Do not create an inbound firewall exception.
- Restrict the release runner and signing key to protected environments with reviewer approval. Rotate update-signing certificates by compiling both current and next fingerprints before the old certificate expires.
- Build the WebRTC DLL with exactly Go 1.26.5, verify the locked Go module graph, and run source-aware `govulncheck` so findings are restricted to reachable imported code. Release packaging rejects older Go runtimes.
- Replace OpenSSL with a reviewed build at or above the locked minimum and record exact SHA-256 hashes for both DLLs. Blank hashes are intentional release blockers.
- Retain redacted operational logs for the minimum useful period. Never log pairing codes, room/host/session IDs, authorization headers, full request URLs, TURN credentials, or Redis errors containing endpoints.

## Abuse and failure handling

- The tray **Stop host** action and emergency hotkey revoke active input and end streaming locally.
- Workstation lock/logoff ends the session and releases simulated input state.
- Pairing tokens are short-lived, role-bound, host-bound, session-bound, and replay-protected through Redis.
- If Redis or the distributed limiter is unavailable, authentication and allocation fail closed.
- The updater rejects HTTP, cross-origin redirects, invalid CMS signatures, untrusted certificate fingerprints, hash mismatches, downgrades, and unsupported Windows versions.

## Residual operational risks

Code cannot eliminate compromise of the Windows account, malicious screen contents, a stolen code-signing key, an unpatched reverse proxy/Redis instance, or secrets copied outside their intended stores. Production readiness therefore still requires infrastructure hardening, key custody, monitoring, incident response, backups, penetration testing, and routine dependency updates.
