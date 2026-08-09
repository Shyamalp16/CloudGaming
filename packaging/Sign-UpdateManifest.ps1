[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string] $ManifestPath,
    [Parameter(Mandatory = $true)][string] $CertificateThumbprint,
    [string] $SignaturePath = "$ManifestPath.p7s"
)

$ErrorActionPreference = 'Stop'
$normalized = $CertificateThumbprint.Replace(' ', '').ToUpperInvariant()
$certificate = Get-ChildItem Cert:\CurrentUser\My, Cert:\LocalMachine\My |
    Where-Object { $_.Thumbprint.Replace(' ', '').ToUpperInvariant() -eq $normalized } |
    Select-Object -First 1
if (-not $certificate) { throw "Signing certificate was not found: $CertificateThumbprint" }
if (-not $certificate.HasPrivateKey) { throw 'The signing certificate does not have an accessible private key.' }

$content = [System.IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $ManifestPath))
$contentInfo = [System.Security.Cryptography.Pkcs.ContentInfo]::new($content)
$signedCms = [System.Security.Cryptography.Pkcs.SignedCms]::new($contentInfo, $true)
$signer = [System.Security.Cryptography.Pkcs.CmsSigner]::new($certificate)
$signer.IncludeOption = [System.Security.Cryptography.X509Certificates.X509IncludeOption]::EndCertOnly
$signedCms.ComputeSignature($signer)
[System.IO.File]::WriteAllBytes([System.IO.Path]::GetFullPath($SignaturePath), $signedCms.Encode())
Write-Host "Detached manifest signature: $SignaturePath"
