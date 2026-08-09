@echo off
setlocal
title Start CloudGaming Development Environment

"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -File "%~dp0packaging\Start-LocalDevelopment.ps1"
if errorlevel 1 (
    echo.
    echo CloudGaming startup failed. Review the error above.
    pause
    exit /b 1
)

echo CloudGaming development services started successfully.
endlocal
