# Cloud Gaming Host installer

The WiX project produces a per-machine x64 MSI with major-upgrade support. It
installs the host and all media/security runtimes, creates a Start menu shortcut,
adds a program-scoped Windows Firewall rule, and removes that rule and the
current user's generated configuration during uninstall.

Build a release payload and installer from a Developer PowerShell prompt:

```powershell
.\packaging\Build-Package.ps1 -Version 0.1.0 -BuildInstaller
```

The build intentionally fails when any declared runtime DLL is missing. Release
automation must Authenticode-sign the EXE and MSI before publishing them.

The release workflow creates a SHA-256 update manifest, signs it as detached
PKCS#7 with the Windows release certificate, and publishes both files. Configure
`update.feedUrl` and `update.publisherCertificateSha256` in the installed
template to enable the tray application's Check updates action. The host rejects
unsigned feeds, unpinned signers, hash mismatches, non-HTTPS downloads, and MSI
files that fail Windows publisher verification.
