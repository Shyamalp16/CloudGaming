[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][ValidatePattern('^\d+\.\d+\.\d+$')][string] $Version,
    [Parameter(Mandatory = $true)][string] $InstallerPath,
    [Parameter(Mandatory = $true)][ValidatePattern('^https://')][string] $DownloadUrl,
    [ValidateSet('stable', 'beta')][string] $Channel = 'stable',
    [string] $OutputPath = "$PSScriptRoot\..\artifacts\update-manifest.json"
)

$ErrorActionPreference = 'Stop'
$downloadUri = $null
if (-not [Uri]::TryCreate($DownloadUrl, [UriKind]::Absolute, [ref]$downloadUri) -or
	$downloadUri.Scheme -ne 'https' -or $downloadUri.UserInfo -or $downloadUri.Fragment) {
	throw 'DownloadUrl must be a credential-free absolute HTTPS URL without a fragment.'
}
$installer = Get-Item -LiteralPath $InstallerPath
$manifest = [ordered]@{
    schemaVersion = 1
    product = 'Cloud Gaming Host'
    version = $Version
    channel = $Channel
    publishedUtc = [DateTime]::UtcNow.ToString('o')
    downloadUrl = $DownloadUrl
    size = $installer.Length
    sha256 = (Get-FileHash -LiteralPath $installer.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    minimumOs = '10.0.19041'
}
$parent = Split-Path -Parent $OutputPath
New-Item -ItemType Directory -Path $parent -Force | Out-Null
$manifest | ConvertTo-Json | Set-Content -LiteralPath $OutputPath -Encoding utf8NoBOM
Write-Host "Update manifest: $OutputPath"
Write-Host 'Sign this manifest with the release certificate and publish the detached .p7s alongside it.'
