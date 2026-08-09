# Cloud Gaming Host installer

The WiX project produces a per-user x64 MSI with major-upgrade support. It
installs the host and all media/security runtimes, creates a Start menu shortcut,
and removes that user's generated host data during uninstall. Installation and
signed updates do not request administrator rights. The host only makes outbound
connections, so the installer intentionally creates no inbound firewall rule.

Build a release payload and installer from a Developer PowerShell prompt:

```powershell
.\packaging\Build-Package.ps1 -Version 0.1.0 -BuildInstaller `
  -CertificateThumbprint $env:SIGNING_CERT_THUMBPRINT `
  -UpdateFeedUrl https://updates.example.com/stable/update-manifest.json
```

The build intentionally fails when any declared runtime DLL is missing. Release
automation must Authenticode-sign every EXE/DLL and the MSI before publishing
them. Unsigned development builds cannot produce an installer.

Release packaging also requires the exact Go 1.26.5 toolchain and verifies that
the compiled WebRTC DLL contains that runtime. Replace both OpenSSL DLLs with a
reviewed build at or above the minimum in `packaging/dependency-lock.json`, then
record their exact file versions and SHA-256 hashes in that lock. Blank hashes
deliberately block release packaging. Never copy toolchains or replacement DLLs
from an untrusted download or from inside the source tree.

The release workflow creates a SHA-256 update manifest, signs it as detached
PKCS#7 with the Windows release certificate, and publishes both files. Configure
the signed executable with the feed URL and current/next publisher certificate
pins to enable the tray application's Check updates action. The host rejects
unsigned feeds, unpinned signers, hash mismatches, non-HTTPS downloads, and MSI
files that fail Windows publisher verification. Installed configuration cannot
override these compiled trust roots.

Use a current self-hosted GitHub Actions runner that supports the Node 24 action
runtime. Protect the release environment with required reviewers and keep the
signing private key non-exportable when the certificate provider supports it.
