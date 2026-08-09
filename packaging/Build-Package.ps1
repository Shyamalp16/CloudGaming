[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^\d+\.\d+\.\d+$')]
    [string] $Version,
    [string] $Configuration = 'Release',
    [string] $Platform = 'x64',
    [string] $OutputRoot = "$PSScriptRoot\..\artifacts",
    [switch] $SkipBuild,
    [switch] $BuildInstaller,
    [string] $CertificateThumbprint,
    [string] $TimestampUrl = 'http://timestamp.digicert.com'
)

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path "$PSScriptRoot\..").Path
$outputRootPath = [System.IO.Path]::GetFullPath($OutputRoot)
$stage = Join-Path $outputRootPath "CloudGamingHost-$Version-$Platform"
$buildOutput = Join-Path $projectRoot "$Platform\$Configuration"

if (-not $SkipBuild) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Installer (vswhere.exe) was not found.' }
    $msbuild = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
    if (-not $msbuild) { throw 'MSBuild was not found.' }
    & $msbuild (Join-Path $projectRoot 'DisplayCaptureProject.sln') /m "/p:Configuration=$Configuration" "/p:Platform=$Platform" /nologo /v:minimal
    if ($LASTEXITCODE -ne 0) { throw "MSBuild failed with exit code $LASTEXITCODE." }
}

$binaryVersion = (Get-Item -LiteralPath (Join-Path $buildOutput 'DisplayCaptureProject.exe')).VersionInfo.ProductVersion
if ($binaryVersion -ne $Version) { throw "Requested package version $Version does not match binary version $binaryVersion." }

if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage -Force | Out-Null

$runtimeManifest = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'runtime-files.json') -Raw | ConvertFrom-Json
foreach ($name in $runtimeManifest.required) {
    $source = Join-Path $buildOutput $name
    if (-not (Test-Path -LiteralPath $source)) { throw "Required runtime file is missing: $source" }
    Copy-Item -LiteralPath $source -Destination (Join-Path $stage $name)
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

$signtool = $null
if ($CertificateThumbprint) {
    $signtool = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin" -Recurse -Filter signtool.exe |
        Where-Object FullName -Match '\\x64\\' | Sort-Object FullName -Descending | Select-Object -First 1
    if (-not $signtool) { throw 'signtool.exe was not found.' }
    & $signtool.FullName sign /sha1 $CertificateThumbprint /fd SHA256 /tr $TimestampUrl /td SHA256 (Join-Path $stage 'DisplayCaptureProject.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Executable signing failed.' }
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
    files = @($files)
}
$packageManifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $stage 'package-manifest.json') -Encoding utf8NoBOM

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
    }
}

Write-Host "Portable package: $zip"
Write-Host "Staging directory: $stage"
