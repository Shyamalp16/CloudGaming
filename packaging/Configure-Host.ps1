[CmdletBinding()]
param(
    [ValidateSet('local', 'production')][string] $Mode = 'local',
    [string] $SignalingUrl,
    [string] $MatchmakerUrl,
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

    $secure = Read-Host 'Host authentication secret' -AsSecureString
    $pointer = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
    try {
        $plain = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($pointer)
        $plain | & $HostExecutable --set-secret hostSecret
        if ($LASTEXITCODE -ne 0) { throw 'Protected host secret storage failed.' }
    } finally {
        if ($plain) { $plain = $null }
        [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($pointer)
    }
} else {
    Write-Host 'Local mode is generated automatically on first launch.'
}

Write-Host 'Configuration is stored under %LOCALAPPDATA%\CloudGamingHost with a user-only ACL.'
Write-Host 'Secrets are encrypted with Windows DPAPI and are never written to config.json.'
