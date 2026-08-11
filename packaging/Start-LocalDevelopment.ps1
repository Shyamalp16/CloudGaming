[CmdletBinding()]
param(
	[switch] $StartStandaloneHost,
	[switch] $NoBrowser
)

$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$serverDirectory = Join-Path $projectRoot 'Server'
$clientDirectory = [IO.Path]::GetFullPath((Join-Path $projectRoot '..\ReflexClient'))
$hostExecutable = Join-Path $projectRoot 'x64\Release\DisplayCaptureProject.exe'
$node = (Get-Command node.exe -ErrorAction Stop).Source
$npm = (Get-Command npm.cmd -ErrorAction Stop).Source

function Test-LocalPort([int] $Port) {
	$client = [Net.Sockets.TcpClient]::new()
	try {
		$task = $client.ConnectAsync('127.0.0.1', $Port)
		return $task.Wait(250) -and $client.Connected
	} catch {
		return $false
	} finally {
		$client.Dispose()
	}
}

function Wait-LocalPort([int] $Port, [int] $TimeoutSeconds = 15) {
	$deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
	do {
		if (Test-LocalPort $Port) { return }
		Start-Sleep -Milliseconds 250
	} while ([DateTime]::UtcNow -lt $deadline)
	throw "Local service did not listen on port $Port within $TimeoutSeconds seconds. Check its console window."
}

function Quote-PowerShellLiteral([string] $Value) {
	return "'" + $Value.Replace("'", "''") + "'"
}

function Start-ServiceConsole([string] $Title, [string] $WorkingDirectory, [string] $Command) {
	$directoryLiteral = Quote-PowerShellLiteral $WorkingDirectory
	$titleLiteral = Quote-PowerShellLiteral $Title
	$windowScript = @"
`$Host.UI.RawUI.WindowTitle = $titleLiteral
Set-Location -LiteralPath $directoryLiteral
$Command
Write-Host ''
Write-Host 'Service stopped. Review the output above, then close this window.' -ForegroundColor Yellow
"@
	$encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($windowScript))
	return Start-Process -FilePath 'powershell.exe' -ArgumentList @(
		'-NoLogo', '-NoProfile', '-NoExit', '-EncodedCommand', $encoded
	) -WindowStyle Normal -PassThru
}

foreach ($port in 3000, 3002, 8080) {
	if (Test-LocalPort $port) {
		throw "Port $port is already in use. Stop the existing local stack before launching another copy."
	}
}

Push-Location $serverDirectory
try {
	& $node -e "require('./config')"
	if ($LASTEXITCODE -ne 0) { throw 'The protected Server/.env configuration is invalid.' }
} finally {
	Pop-Location
}

if (-not (Test-LocalPort 6379)) {
	$wsl = Get-Command wsl.exe -ErrorAction Stop
	& $wsl.Source sh -lc 'command -v redis-server >/dev/null'
	if ($LASTEXITCODE -ne 0) { throw 'Redis is not installed in the default WSL distribution.' }
	Start-ServiceConsole 'Cloud Gaming - Redis' $projectRoot `
		'& wsl.exe sh -lc ''exec redis-server --bind 127.0.0.1 --protected-mode yes --save "" --appendonly no''' | Out-Null
	Wait-LocalPort 6379
} else {
	Write-Host 'Using the Redis instance already listening on 127.0.0.1:6379.'
}

$nodeLiteral = Quote-PowerShellLiteral $node
Start-ServiceConsole 'Cloud Gaming - Signaling' $serverDirectory `
	"`$env:PRETTY_LOGS='true'; & $nodeLiteral 'ScalableSignalingServer.js'" | Out-Null
Wait-LocalPort 3002
$health = Invoke-WebRequest -UseBasicParsing -Uri 'http://127.0.0.1:3002/healthz' -TimeoutSec 5
if ($health.StatusCode -ne 200) { throw 'The signaling health check failed.' }

Start-ServiceConsole 'Cloud Gaming - Matchmaker' $serverDirectory `
	"`$env:PRETTY_LOGS='true'; & $nodeLiteral 'mm_server\Matchmaker.js'" | Out-Null
Wait-LocalPort 3000

$npmLiteral = Quote-PowerShellLiteral $npm
Start-ServiceConsole 'Cloud Gaming - Browser' $clientDirectory `
	"& $npmLiteral 'run' 'dev' '--' '--host' '127.0.0.1' '--port' '8080'" | Out-Null
Wait-LocalPort 8080

if ($StartStandaloneHost) {
	if (-not (Test-Path -LiteralPath $hostExecutable -PathType Leaf)) {
		throw "Release host executable not found: $hostExecutable"
	}
	Start-Process -FilePath $hostExecutable -WorkingDirectory (Split-Path -Parent $hostExecutable) | Out-Null
}

if (-not $NoBrowser) {
	Start-Process 'http://localhost:8080' | Out-Null
}

Write-Host 'Local cloud-gaming stack is ready.' -ForegroundColor Green
Write-Host 'Use Reflex Desktop to start hosting. Pass -StartStandaloneHost only for legacy host testing.'
Write-Host 'Service logs remain visible in their console windows. Close those windows to stop the stack.'
