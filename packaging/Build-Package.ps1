[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^\d+\.\d+\.\d+$')]
    [string] $Version,
    [ValidateSet('Debug', 'Release')][string] $Configuration = 'Release',
    [ValidateSet('x64')][string] $Platform = 'x64',
    [string] $OutputRoot = "$PSScriptRoot\..\artifacts",
    [switch] $SkipBuild,
    [switch] $BuildInstaller,
	[ValidatePattern('^[a-fA-F0-9]{40}$')][string] $CertificateThumbprint,
	[ValidatePattern('^https://')][string] $UpdateFeedUrl,
	[ValidatePattern('^[a-fA-F0-9]{64}$')][string] $UpdateCertificateSha256Next,
	[switch] $AllowUnsignedDevelopment,
	[ValidatePattern('^https://')][string] $TimestampUrl = 'https://timestamp.digicert.com',
	[string] $GccPath = 'C:\msys64\mingw64\bin\gcc.exe'
)

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path "$PSScriptRoot\..").Path
$outputRootPath = [System.IO.Path]::GetFullPath($OutputRoot)
$stage = Join-Path $outputRootPath "CloudGamingHost-$Version-$Platform"
$buildOutput = Join-Path $projectRoot "$Platform\$Configuration"
$sourceCommit = (& git -C $projectRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $sourceCommit -notmatch '^[a-f0-9]{40}$') { throw 'Could not identify the source commit.' }

if ($Configuration -eq 'Release' -and -not $CertificateThumbprint -and -not $AllowUnsignedDevelopment) {
	throw 'Release packaging requires -CertificateThumbprint. Use -AllowUnsignedDevelopment only for local, non-distributable builds.'
}
if ($BuildInstaller -and $AllowUnsignedDevelopment) {
	throw 'Installer creation is blocked for unsigned development packages.'
}
if ($Configuration -eq 'Release' -and -not $AllowUnsignedDevelopment) {
	$dirty = & git -C $projectRoot status --porcelain
	if ($dirty) { throw 'Release packaging requires a clean committed source tree.' }
}

function Assert-CredentialFreeHttpsUri([string] $Value, [string] $Name) {
	$uri = $null
	if (-not [Uri]::TryCreate($Value, [UriKind]::Absolute, [ref]$uri) -or $uri.Scheme -ne 'https' -or
		$uri.UserInfo -or $uri.Fragment -or $Value.Contains('"') -or $Value.Contains(';')) {
		throw "$Name must be a credential-free absolute HTTPS URL without fragments, quotes, or semicolons."
	}
}
if ($UpdateFeedUrl) { Assert-CredentialFreeHttpsUri $UpdateFeedUrl 'UpdateFeedUrl' }
Assert-CredentialFreeHttpsUri $TimestampUrl 'TimestampUrl'

$releaseCertificate = $null
$updateCertificateSha256 = ''
if ($CertificateThumbprint) {
	$normalizedThumbprint = $CertificateThumbprint.Replace(' ', '').ToUpperInvariant()
	$releaseCertificate = Get-ChildItem Cert:\CurrentUser\My, Cert:\LocalMachine\My |
		Where-Object { $_.Thumbprint.Replace(' ', '').ToUpperInvariant() -eq $normalizedThumbprint } |
		Select-Object -First 1
	if (-not $releaseCertificate -or -not $releaseCertificate.HasPrivateKey) { throw 'The release signing certificate is unavailable.' }
	$updateCertificateSha256 = [Convert]::ToHexString(
		[Security.Cryptography.SHA256]::HashData($releaseCertificate.RawData)).ToLowerInvariant()
	if (-not $UpdateFeedUrl -and -not $AllowUnsignedDevelopment) { throw 'Signed releases require -UpdateFeedUrl.' }
}

if (-not $SkipBuild) {
	$goSource = Join-Path $projectRoot 'gortc_main'
	$goDll = Join-Path $goSource 'webrtc.dll'
	if (-not (Test-Path -LiteralPath (Join-Path $goSource 'go.mod')) -or
		-not (Test-Path -LiteralPath (Join-Path $goSource 'go.sum'))) {
		throw 'Go module lock files are required for the WebRTC DLL build.'
	}
	$gccPath = [IO.Path]::GetFullPath($GccPath)
	if (-not (Test-Path -LiteralPath $gccPath -PathType Leaf) -or
		$gccPath.StartsWith($projectRoot, [StringComparison]::OrdinalIgnoreCase)) {
		throw 'A trusted MinGW-w64 GCC outside the source tree is required.'
	}
	$goCommand = Get-Command go.exe -CommandType Application -ErrorAction Stop
	$goPath = [IO.Path]::GetFullPath($goCommand.Source)
	if ($goPath.StartsWith($projectRoot, [StringComparison]::OrdinalIgnoreCase)) {
		throw 'The Go toolchain must be installed outside the source tree.'
	}
	$oldCgo = $env:CGO_ENABLED
	$oldCc = $env:CC
	$oldPath = $env:Path
	Push-Location -LiteralPath $goSource
	try {
		$goVersion = (& $goPath env GOVERSION).Trim()
		if ($LASTEXITCODE -ne 0 -or $goVersion -ne 'go1.26.5') {
			throw "Release builds require the security-patched Go 1.26.5 toolchain; found $goVersion."
		}
		$env:CGO_ENABLED = '1'
		$env:CC = $gccPath
		$env:Path = "$(Split-Path -Parent $gccPath);$oldPath"
		& $goPath mod verify
		if ($LASTEXITCODE -ne 0) { throw 'Go module checksum verification failed.' }
		& $goPath build -mod=readonly -buildvcs=true -trimpath -buildmode=c-shared `
			-ldflags '-s -w -buildid=' -o $goDll .
		if ($LASTEXITCODE -ne 0) { throw 'Go WebRTC DLL build failed.' }
	} finally {
		$env:CGO_ENABLED = $oldCgo
		$env:CC = $oldCc
		$env:Path = $oldPath
		Pop-Location
	}
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Installer (vswhere.exe) was not found.' }
    $msbuild = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
    if (-not $msbuild) { throw 'MSBuild was not found.' }
	& $msbuild (Join-Path $projectRoot 'DisplayCaptureProject.sln') /m "/p:Configuration=$Configuration" "/p:Platform=$Platform" `
		"/p:UpdateFeedUrl=$UpdateFeedUrl" "/p:UpdateCertificateSha256=$updateCertificateSha256" `
		"/p:UpdateCertificateSha256Next=$UpdateCertificateSha256Next" /nologo /v:minimal
    if ($LASTEXITCODE -ne 0) { throw "MSBuild failed with exit code $LASTEXITCODE." }
}

$binaryVersion = (Get-Item -LiteralPath (Join-Path $buildOutput 'DisplayCaptureProject.exe')).VersionInfo.ProductVersion
if ($binaryVersion -ne $Version) { throw "Requested package version $Version does not match binary version $binaryVersion." }

if (-not $stage.StartsWith($outputRootPath + [IO.Path]::DirectorySeparatorChar,
	[System.StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe staging directory resolution.' }
if (Test-Path -LiteralPath $stage) {
	$existingStage = Get-Item -LiteralPath $stage -Force
	if ($existingStage.Attributes -band [IO.FileAttributes]::ReparsePoint) {
		throw 'Refusing to clean a staging path that is a reparse point.'
	}
	Remove-Item -LiteralPath $stage -Recurse -Force
}
New-Item -ItemType Directory -Path $stage -Force | Out-Null

$runtimeManifest = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'runtime-files.json') -Raw | ConvertFrom-Json
$dependencyLock = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'dependency-lock.json') -Raw | ConvertFrom-Json
if ($dependencyLock.schemaVersion -ne 1) { throw 'Unsupported dependency lock schema.' }
$lockedDependencies = @{}
foreach ($entry in $dependencyLock.dependencies) { $lockedDependencies[$entry.name] = $entry }
foreach ($name in $runtimeManifest.required) {
    $source = Join-Path $buildOutput $name
    if (-not (Test-Path -LiteralPath $source)) { throw "Required runtime file is missing: $source" }
	if ($name -ne 'DisplayCaptureProject.exe' -and $name -ne 'webrtc.dll') {
		$lock = $lockedDependencies[$name]
		if (-not $lock) { throw "No dependency lock exists for $name." }
		if ($lock.sha256 -notmatch '^[a-f0-9]{64}$') {
			throw "Dependency $name has not been approved. Upgrade it, review its provenance, and record its SHA-256 in packaging/dependency-lock.json."
		}
		$actualHash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant()
		if ($actualHash -ne $lock.sha256) { throw "Dependency hash mismatch: $name" }
		if ($lock.minimumVersion) {
			$actualVersion = (Get-Item -LiteralPath $source).VersionInfo.FileVersion
			if (-not $actualVersion -or [version]$actualVersion -lt [version]$lock.minimumVersion) {
				throw "Dependency $name must be at least version $($lock.minimumVersion); found $actualVersion."
			}
		}
	}
    Copy-Item -LiteralPath $source -Destination (Join-Path $stage $name)
}

$goInspector = [IO.Path]::GetFullPath((Get-Command go.exe -CommandType Application -ErrorAction Stop).Source)
if ($goInspector.StartsWith($projectRoot, [StringComparison]::OrdinalIgnoreCase)) {
	throw 'The Go inspection tool must be installed outside the source tree.'
}
$goMetadata = & $goInspector version -m (Join-Path $buildOutput 'webrtc.dll') 2>&1
if ($LASTEXITCODE -ne 0) { throw 'Could not inspect the Go/WebRTC build metadata.' }
if (@($goMetadata)[0] -notmatch ': go1\.26\.5$') {
	throw 'The WebRTC DLL was not built with the required security-patched Go 1.26.5 runtime.'
}
if ($goMetadata -match 'vcs.modified=true' -and -not $AllowUnsignedDevelopment) {
	throw 'The Go/WebRTC DLL was built from a dirty source tree.'
}
if ($goMetadata -match 'github\.com/pion/(webrtc/v3|dtls/v2)') {
	throw 'The WebRTC DLL contains an unsupported Pion v3/DTLS v2 dependency.'
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsRoot = & $vswhere -latest -products * -property installationPath | Select-Object -First 1
$redistRoot = Get-ChildItem -LiteralPath (Join-Path $vsRoot 'VC\Redist\MSVC') -Directory |
    Where-Object Name -Match '^\d+\.' | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (-not $redistRoot) { throw 'Visual C++ redistributable directory was not found.' }
$crt = Join-Path $redistRoot.FullName 'x64\Microsoft.VC143.CRT'
foreach ($name in $runtimeManifest.visualCppRuntime) {
    $source = Join-Path $crt $name
    if (-not (Test-Path -LiteralPath $source)) { throw "Required Visual C++ runtime file is missing: $source" }
    Copy-Item -LiteralPath $source -Destination (Join-Path $stage $name)
}

Copy-Item -LiteralPath (Join-Path $projectRoot 'config.json') -Destination (Join-Path $stage 'config.json')
Copy-Item -LiteralPath (Join-Path $projectRoot 'LICENSE.md') -Destination (Join-Path $stage 'LICENSE.md')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'THIRD_PARTY_NOTICES.md') -Destination (Join-Path $stage 'THIRD_PARTY_NOTICES.md')
Copy-Item -LiteralPath (Join-Path $projectRoot 'Server\.env.example') -Destination (Join-Path $stage 'server.env.example')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'Configure-Host.ps1') -Destination $stage
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'New-ServerEnvironment.ps1') -Destination $stage

$components = foreach ($entry in $dependencyLock.dependencies) {
	$component = [ordered]@{
		type = 'library'
		name = $entry.name
		version = if ($entry.version) { $entry.version } else { $entry.minimumVersion }
		hashes = @([ordered]@{ alg = 'SHA-256'; content = $entry.sha256 })
	}
	$component
}
$goComponents = foreach ($line in $goMetadata) {
	if ($line -match '^\s*dep\s+(\S+)\s+(\S+)\s+(\S+)$') {
		[ordered]@{
			type = 'library'
			name = $Matches[1]
			version = $Matches[2]
			properties = @([ordered]@{ name = 'go.sum'; value = $Matches[3] })
		}
	}
}
$sbom = [ordered]@{
	bomFormat = 'CycloneDX'
	specVersion = '1.5'
	serialNumber = "urn:uuid:$([guid]::NewGuid())"
	version = 1
	metadata = [ordered]@{ component = [ordered]@{ type = 'application'; name = 'Cloud Gaming Host'; version = $Version } }
	components = @($components) + @($goComponents)
}
$sbom | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $stage 'sbom.cdx.json') -Encoding utf8NoBOM

$signtool = $null
if ($CertificateThumbprint) {
    $signtool = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin" -Recurse -Filter signtool.exe |
        Where-Object FullName -Match '\\x64\\' | Sort-Object FullName -Descending | Select-Object -First 1
    if (-not $signtool) { throw 'signtool.exe was not found.' }
	$signTargets = Get-ChildItem -LiteralPath $stage -File | Where-Object Extension -In '.exe', '.dll'
	foreach ($target in $signTargets) {
		& $signtool.FullName sign /sha1 $CertificateThumbprint /fd SHA256 /tr $TimestampUrl /td SHA256 $target.FullName
		if ($LASTEXITCODE -ne 0) { throw "Signing failed: $($target.Name)" }
		& $signtool.FullName verify /pa /all $target.FullName
		if ($LASTEXITCODE -ne 0) { throw "Signature verification failed: $($target.Name)" }
	}
}

$files = Get-ChildItem -LiteralPath $stage -File | Sort-Object Name | ForEach-Object {
    [ordered]@{
        path = $_.Name
        size = $_.Length
        sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}
$packageManifest = [ordered]@{
    schemaVersion = 1
    product = 'Cloud Gaming Host'
    version = $Version
    architecture = $Platform
    createdUtc = [DateTime]::UtcNow.ToString('o')
	sourceCommit = $sourceCommit
    files = @($files)
}
$packageManifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $stage 'package-manifest.json') -Encoding utf8NoBOM

if ($CertificateThumbprint) {
	& (Join-Path $PSScriptRoot 'Sign-UpdateManifest.ps1') -ManifestPath (Join-Path $stage 'package-manifest.json') `
		-CertificateThumbprint $CertificateThumbprint -SignaturePath (Join-Path $stage 'package-manifest.json.p7s')
}

$zip = "$stage.zip"
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -CompressionLevel Optimal

if ($BuildInstaller) {
    $wixProject = Join-Path $projectRoot 'Installer\CloudGamingHost.Installer.wixproj'
    & dotnet build $wixProject -c Release "/p:ProductVersion=$Version" "/p:PayloadDir=$stage"
    if ($LASTEXITCODE -ne 0) { throw "Installer build failed with exit code $LASTEXITCODE." }
	if ($CertificateThumbprint) {
        $installer = Join-Path $projectRoot "Installer\bin\x64\Release\CloudGamingHost-$Version-x64.msi"
        & $signtool.FullName sign /sha1 $CertificateThumbprint /fd SHA256 /tr $TimestampUrl /td SHA256 $installer
        if ($LASTEXITCODE -ne 0) { throw 'Installer signing failed.' }
		& $signtool.FullName verify /pa /all $installer
		if ($LASTEXITCODE -ne 0) { throw 'Installer signature verification failed.' }
    }
}

Write-Host "Portable package: $zip"
Write-Host "Staging directory: $stage"
