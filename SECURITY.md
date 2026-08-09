# Security policy

## Supported versions

Only the most recent signed stable release is supported. Beta builds are for testing and may be withdrawn without notice.

## Reporting a vulnerability

Do not open a public issue containing exploit details, pairing codes, credentials, logs, or host identifiers. Report the issue privately through the repository host's private vulnerability-reporting feature. Include the affected version, reproduction steps, impact, and whether the issue has been exploited. If private reporting is not enabled, the repository owner must enable it before public distribution.

The project owner should acknowledge a report within three business days, rotate any exposed credentials immediately, and publish a signed update after validation. Public disclosure should wait until a fix is available.

## Release requirements

A release is valid only when all of the following are true:

- CI and security workflows pass on the release commit.
- The Windows executable, DLLs, and MSI are Authenticode-signed and timestamped.
- `packaging/dependency-lock.json` has exact versions and SHA-256 hashes for every bundled native dependency.
- The update manifest and installer are signed by a certificate whose SHA-256 fingerprint is compiled into the host.
- The build runs from a clean, reviewed commit on the isolated Windows release runner.
- Production secrets are injected by the deployment platform and never committed or included in artifacts.

Never distribute a locally built or unsigned package as a production release.
