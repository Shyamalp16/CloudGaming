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
if ([DateTime]::UtcNow -lt $certificate.NotBefore.ToUniversalTime() -or
	[DateTime]::UtcNow -gt $certificate.NotAfter.ToUniversalTime()) { throw 'The signing certificate is not currently valid.' }
$codeSigningEku = $certificate.Extensions | Where-Object { $_.Oid.Value -eq '2.5.29.37' } |
	ForEach-Object { $_.EnhancedKeyUsages | Where-Object Value -eq '1.3.6.1.5.5.7.3.3' }
if (-not $codeSigningEku) { throw 'The certificate is not authorized for code signing.' }
$chain = [Security.Cryptography.X509Certificates.X509Chain]::new()
$chain.ChainPolicy.RevocationMode = [Security.Cryptography.X509Certificates.X509RevocationMode]::Online
$chain.ChainPolicy.RevocationFlag = [Security.Cryptography.X509Certificates.X509RevocationFlag]::EntireChain
if (-not $chain.Build($certificate)) { throw 'The signing certificate chain or revocation status is invalid.' }

$content = [System.IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $ManifestPath))
$contentInfo = [System.Security.Cryptography.Pkcs.ContentInfo]::new($content)
$signedCms = [System.Security.Cryptography.Pkcs.SignedCms]::new($contentInfo, $true)
$signer = [System.Security.Cryptography.Pkcs.CmsSigner]::new($certificate)
$signer.IncludeOption = [System.Security.Cryptography.X509Certificates.X509IncludeOption]::EndCertOnly
$signer.DigestAlgorithm = [Security.Cryptography.Oid]::new('2.16.840.1.101.3.4.2.1')
$signedCms.ComputeSignature($signer)
[System.IO.File]::WriteAllBytes([System.IO.Path]::GetFullPath($SignaturePath), $signedCms.Encode())
Write-Host "Detached manifest signature: $SignaturePath"
