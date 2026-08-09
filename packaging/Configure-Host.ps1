[CmdletBinding()]
param(
    [ValidateSet('local', 'production')][string] $Mode = 'local',
    [string] $SignalingUrl,
	[string] $MatchmakerUrl,
	[string] $HostCredentialFile,
	[switch] $KeepCredentialFile,
    [string] $HostExecutable = "$PSScriptRoot\DisplayCaptureProject.exe"
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $HostExecutable)) { throw "Host executable not found: $HostExecutable" }

if ($Mode -eq 'production') {
    if ($SignalingUrl -notmatch '^wss://' -or $MatchmakerUrl -notmatch '^https://') {
        throw 'Production requires a wss:// signaling URL and an https:// matchmaker URL.'
    }
    & $HostExecutable --configure-production $SignalingUrl $MatchmakerUrl
    if ($LASTEXITCODE -ne 0) { throw 'Production endpoint configuration failed.' }

} else {
	Write-Host 'Keeping local network endpoints.'
}

$deviceOutput = @(& $HostExecutable --device-id 2>&1)
if ($LASTEXITCODE -ne 0) { throw 'Could not load the stable host device identity.' }
$deviceIds = @($deviceOutput | ForEach-Object {
	if ([string]$_ -match '^\s*([0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[1-5][0-9a-fA-F]{3}-[89aAbB][0-9a-fA-F]{3}-[0-9a-fA-F]{12})\s*$') {
		$Matches[1].ToLowerInvariant()
	}
} | Select-Object -Unique)
if ($deviceIds.Count -ne 1) { throw 'Could not load exactly one stable host device identity.' }
$deviceId = $deviceIds[0]
if ($HostCredentialFile) {
	$credential = Get-Content -LiteralPath $HostCredentialFile -Raw | ConvertFrom-Json
	if ($credential.hostId -ne $deviceId) { throw 'The credential file belongs to a different host device ID.' }
	$plain = [string]$credential.hostSecret
	if ($plain.Length -lt 32) { throw 'The host credential is invalid.' }
	$plain | & $HostExecutable --set-secret hostSecret
	$plain = $null
	if ($LASTEXITCODE -ne 0) { throw 'Protected host secret storage failed.' }
	if (-not $KeepCredentialFile) {
		Remove-Item -LiteralPath ([IO.Path]::GetFullPath($HostCredentialFile)) -Force
		Write-Host 'Removed the plaintext credential transfer file after DPAPI import.'
	}
} else {
	Write-Host "Stable host device ID: $deviceId"
	$secure = Read-Host 'Per-host authentication secret from HOST_CREDENTIALS_JSON' -AsSecureString
	$pointer = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
	try {
		$plain = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($pointer)
		$plain | & $HostExecutable --set-secret hostSecret
		if ($LASTEXITCODE -ne 0) { throw 'Protected host secret storage failed.' }
	} finally {
		if ($plain) { $plain = $null }
		[Runtime.InteropServices.Marshal]::ZeroFreeBSTR($pointer)
	}
}

Write-Host 'Configuration is stored under %LOCALAPPDATA%\CloudGamingHost with a user-only ACL.'
Write-Host 'Secrets are encrypted with Windows DPAPI and are never written to config.json.'
