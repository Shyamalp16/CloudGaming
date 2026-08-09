[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string] $OutputPath,
    [ValidateSet('development', 'production')][string] $Environment = 'production',
    [string] $RedisUrl = 'redis://127.0.0.1:6379',
    [string] $AllowedOrigins = ''
)

$ErrorActionPreference = 'Stop'
function New-Secret([int] $ByteCount = 48) {
    $bytes = [byte[]]::new($ByteCount)
    [Security.Cryptography.RandomNumberGenerator]::Fill($bytes)
    return [Convert]::ToBase64String($bytes)
}

$production = $Environment -eq 'production'
$lines = @(
    "NODE_ENV=$Environment",
    "REDIS_URL=$RedisUrl",
    'WS_PORT=3002',
    'MATCHMAKER_PORT=3000',
    "HOST_SECRET=$(New-Secret)",
    "PAIRING_TOKEN_SECRET=$(New-Secret)",
    "ENABLE_SESSION_AUTH=$($production.ToString().ToLowerInvariant())",
    "REQUIRE_WSS=$($production.ToString().ToLowerInvariant())"
)
if ($AllowedOrigins) { $lines += "ALLOWED_ORIGINS=$AllowedOrigins" }
$absolute = [IO.Path]::GetFullPath($OutputPath)
New-Item -ItemType Directory -Path (Split-Path -Parent $absolute) -Force | Out-Null
$lines | Set-Content -LiteralPath $absolute -Encoding utf8NoBOM
$acl = Get-Acl -LiteralPath $absolute
$acl.SetAccessRuleProtection($true, $false)
$identity = [Security.Principal.WindowsIdentity]::GetCurrent().User
$rule = [Security.AccessControl.FileSystemAccessRule]::new($identity, 'FullControl', 'Allow')
$acl.SetAccessRule($rule)
Set-Acl -LiteralPath $absolute -AclObject $acl
Write-Host "Generated protected server environment: $absolute"
