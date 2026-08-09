[CmdletBinding()]
param(
	[switch] $NoHost,
	[switch] $NoBrowser
)

$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$serverDirectory = Join-Path $projectRoot 'Server'
$clientDirectory = Join-Path $projectRoot 'Client\html-server'
$hostExecutable = Join-Path $projectRoot 'x64\Release\DisplayCaptureProject.exe'
$node = (Get-Command node.exe -ErrorAction Stop).Source
$python = (Get-Command py.exe -ErrorAction Stop).Source

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

foreach ($port in 3000, 3002, 8080, 8081) {
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
Wait-LocalPort 8081

Start-ServiceConsole 'Cloud Gaming - Matchmaker' $serverDirectory `
	"`$env:PRETTY_LOGS='true'; & $nodeLiteral 'mm_server\Matchmaker.js'" | Out-Null
Wait-LocalPort 3000

$pythonLiteral = Quote-PowerShellLiteral $python
Start-ServiceConsole 'Cloud Gaming - Browser' $clientDirectory `
	"& $pythonLiteral -m http.server 8080 --bind 127.0.0.1" | Out-Null
Wait-LocalPort 8080

if (-not $NoHost) {
	if (-not (Test-Path -LiteralPath $hostExecutable -PathType Leaf)) {
		throw "Release host executable not found: $hostExecutable"
	}
	Start-Process -FilePath $hostExecutable -WorkingDirectory (Split-Path -Parent $hostExecutable) | Out-Null
}

if (-not $NoBrowser) {
	Start-Process 'http://localhost:8080' | Out-Null
}

Write-Host 'Local cloud-gaming stack is ready.' -ForegroundColor Green
Write-Host 'Service logs remain visible in their console windows. Close those windows to stop the stack.'
