[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string] $OutputPath,
	[Parameter(Mandatory = $true)][ValidatePattern('^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[1-5][0-9a-fA-F]{3}-[89aAbB][0-9a-fA-F]{3}-[0-9a-fA-F]{12}$')][string] $HostId,
    [ValidateSet('development', 'production')][string] $Environment = 'production',
    [string] $RedisUrl = 'redis://127.0.0.1:6379',
	[string] $AllowedOrigins = '',
	[string] $TrustedProxyIps = '',
	[string] $BindHost = '127.0.0.1',
	[string] $MeteredDomain = '',
	[string] $MeteredApiKey = ''
)

$ErrorActionPreference = 'Stop'
function New-Secret([int] $ByteCount = 48) {
    $bytes = [byte[]]::new($ByteCount)
    [Security.Cryptography.RandomNumberGenerator]::Fill($bytes)
    return [Convert]::ToBase64String($bytes)
}

function Write-ProtectedFile([string] $Path, [object] $Content) {
	$absolutePath = [IO.Path]::GetFullPath($Path)
	$directory = Split-Path -Parent $absolutePath
	New-Item -ItemType Directory -Path $directory -Force | Out-Null
	$tempPath = Join-Path $directory ('.' + [IO.Path]::GetFileName($absolutePath) + '.' + [Guid]::NewGuid().ToString('N') + '.tmp')
	try {
		$empty = [IO.FileStream]::new($tempPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
		$empty.Dispose()
		$acl = Get-Acl -LiteralPath $tempPath
		$acl.SetAccessRuleProtection($true, $false)
		$identity = [Security.Principal.WindowsIdentity]::GetCurrent().User
		$rule = [Security.AccessControl.FileSystemAccessRule]::new($identity, 'FullControl', 'Allow')
		$acl.SetAccessRule($rule)
		$systemRule = [Security.AccessControl.FileSystemAccessRule]::new('SYSTEM', 'FullControl', 'Allow')
		$acl.AddAccessRule($systemRule)
		Set-Acl -LiteralPath $tempPath -AclObject $acl
		$Content | Set-Content -LiteralPath $tempPath -Encoding utf8NoBOM
		Move-Item -LiteralPath $tempPath -Destination $absolutePath -Force
	} finally {
		if (Test-Path -LiteralPath $tempPath) { Remove-Item -LiteralPath $tempPath -Force }
	}
}

$production = $Environment -eq 'production'
$hostSecret = New-Secret
$parsedBindHost = $null
if (-not [Net.IPAddress]::TryParse($BindHost, [ref]$parsedBindHost)) {
	throw 'BindHost must be an exact IP address. Prefer loopback or a private service address.'
}
if ($production) {
	$redis = [Uri]$RedisUrl
	if ($redis.Scheme -ne 'rediss' -or -not $redis.UserInfo.Contains(':') -or $redis.Query -or $redis.Fragment) {
		throw 'Production Redis must use rediss:// with username/password and no query or fragment.'
	}
	if (-not $AllowedOrigins) { throw 'Production requires an explicit -AllowedOrigins list.' }
	if (-not $TrustedProxyIps) { throw 'Production requires exact -TrustedProxyIps for TLS forwarding validation.' }
	foreach ($origin in $AllowedOrigins.Split(',')) {
		$uri = [Uri]$origin.Trim()
		if ($uri.Scheme -ne 'https' -or $uri.UserInfo -or $uri.PathAndQuery -ne '/' -or $uri.Fragment) {
			throw "Invalid production origin: $origin"
		}
	}
	foreach ($address in $TrustedProxyIps.Split(',')) {
		$parsedAddress = $null
		if (-not [Net.IPAddress]::TryParse($address.Trim(), [ref]$parsedAddress)) {
			throw "Trusted proxy must be an exact IP address: $address"
		}
	}
	if ($MeteredDomain -notmatch '^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?$' -or $MeteredApiKey.Length -lt 16) {
		throw 'Production requires a Metered tenant name and API key from the secret manager.'
	}
}
$hostCredentials = @{ $HostId.ToLowerInvariant() = $hostSecret } | ConvertTo-Json -Compress
$lines = @(
    "NODE_ENV=$Environment",
    "REDIS_URL=$RedisUrl",
	'REDIS_KEY_PREFIX=cg:v1:',
    'WS_PORT=3002',
    'MATCHMAKER_PORT=3000',
	'HEALTH_PORT=8081',
	"BIND_HOST=$BindHost",
	"HOST_CREDENTIALS_JSON=$hostCredentials",
    "PAIRING_TOKEN_SECRET=$(New-Secret)",
	"METRICS_SECRET=$(New-Secret)",
	'ENABLE_SESSION_AUTH=true',
	'SUBPROTOCOL=cloud-gaming-v1',
	"REQUIRE_WSS=$($production.ToString().ToLowerInvariant())"
)
if ($AllowedOrigins) { $lines += "ALLOWED_ORIGINS=$AllowedOrigins" }
if ($TrustedProxyIps) {
	$lines += "TRUSTED_PROXY_IPS=$TrustedProxyIps"
}
if ($MeteredDomain) { $lines += "METERED_DOMAIN=$MeteredDomain" }
if ($MeteredApiKey) { $lines += "METERED_API_KEY=$MeteredApiKey" }
$absolute = [IO.Path]::GetFullPath($OutputPath)
Write-ProtectedFile -Path $absolute -Content $lines
$credentialPath = "$absolute.host-credential.json"
$credential = @{ hostId = $HostId.ToLowerInvariant(); hostSecret = $hostSecret } | ConvertTo-Json
Write-ProtectedFile -Path $credentialPath -Content $credential
Write-Host "Generated protected server environment: $absolute"
Write-Host "Generated protected host credential for DPAPI import: $credentialPath"
