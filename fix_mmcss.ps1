# MMCSS Diagnostic and Fix Script for DisplayCaptureProject
# This script helps diagnose and fix MMCSS (Multimedia Class Scheduler Service) issues

param(
    [switch]$Diagnose,
    [switch]$Fix,
    [switch]$All
)

function Write-Header {
    param([string]$Message)
    Write-Host "`n==========================================" -ForegroundColor Cyan
    Write-Host " $Message" -ForegroundColor Yellow
    Write-Host "==========================================" -ForegroundColor Cyan
}

function Check-Administrator {
    $currentUser = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($currentUser)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Check-MMCSS-Service {
    Write-Host "Checking MMCSS Service Status..." -ForegroundColor Green

    try {
        $service = Get-Service -Name MMCSS -ErrorAction Stop
        Write-Host "  MMCSS Service: $($service.Status)" -ForegroundColor $(if ($service.Status -eq 'Running') { 'Green' } else { 'Red' })

        if ($service.Status -ne 'Running') {
            Write-Host "  Attempting to start MMCSS service..." -ForegroundColor Yellow
            Start-Service -Name MMCSS
            Start-Sleep -Seconds 2
            $service.Refresh()
            Write-Host "  MMCSS Service: $($service.Status)" -ForegroundColor $(if ($service.Status -eq 'Running') { 'Green' } else { 'Red' })
        }

        # Check startup type
        $startType = Get-WmiObject -Class Win32_Service -Filter "Name='MMCSS'" | Select-Object -ExpandProperty StartMode
        Write-Host "  MMCSS Startup Type: $startType" -ForegroundColor $(if ($startType -eq 'Auto') { 'Green' } else { 'Yellow' })

        if ($startType -ne 'Auto') {
            Write-Host "  Setting MMCSS to Automatic startup..." -ForegroundColor Yellow
            Set-Service -Name MMCSS -StartupType Automatic
            Write-Host "  MMCSS Startup Type: Auto" -ForegroundColor Green
        }

        return $true
    }
    catch {
        Write-Host "  ERROR: Could not query MMCSS service - $($_.Exception.Message)" -ForegroundColor Red
        return $false
    }
}

function Check-PagingFile {
    Write-Host "Checking Page File Configuration..." -ForegroundColor Green

    try {
        $pageFile = Get-WmiObject -Class Win32_PageFileSetting
        if ($pageFile) {
            Write-Host "  Page File Location: $($pageFile.Name)" -ForegroundColor White
            Write-Host "  Initial Size: $($pageFile.InitialSize) MB" -ForegroundColor White
            Write-Host "  Maximum Size: $($pageFile.MaximumSize) MB" -ForegroundColor White

            # Get current page file usage
            $pageFileUsage = Get-WmiObject -Class Win32_PageFileUsage
            if ($pageFileUsage) {
                Write-Host "  Current Usage: $($pageFileUsage.CurrentUsage) MB" -ForegroundColor White
                Write-Host "  Peak Usage: $($pageFileUsage.PeakUsage) MB" -ForegroundColor White
                Write-Host "  Allocated Base Size: $($pageFileUsage.AllocatedBaseSize) MB" -ForegroundColor White
            }

            # Check if page file is system managed
            if ($pageFile.InitialSize -eq 0 -and $pageFile.MaximumSize -eq 0) {
                Write-Host "  Page File: System Managed" -ForegroundColor Green
            } else {
                Write-Host "  Page File: Custom Size" -ForegroundColor Yellow
            }

            return $true
        } else {
            Write-Host "  WARNING: No page file found!" -ForegroundColor Red
            return $false
        }
    }
    catch {
        Write-Host "  ERROR: Could not check page file - $($_.Exception.Message)" -ForegroundColor Red
        return $false
    }
}

function Check-MMCSS-DLLs {
    Write-Host "Checking MMCSS DLL Availability..." -ForegroundColor Green

    $mmcssDlls = @(
        "$env:windir\System32\avrt.dll",
        "$env:windir\SysWOW64\avrt.dll"
    )

    $allPresent = $true

    foreach ($dll in $mmcssDlls) {
        if (Test-Path $dll) {
            $fileInfo = Get-Item $dll
            Write-Host "  [OK] $($dll): Present ($($fileInfo.Length) bytes)" -ForegroundColor Green
        } else {
            Write-Host "  [FAIL] $($dll): NOT FOUND" -ForegroundColor Red
            $allPresent = $false
        }
    }

    return $allPresent
}

function Check-System-Resources {
    Write-Host "Checking System Resources..." -ForegroundColor Green

    try {
        $os = Get-WmiObject -Class Win32_OperatingSystem
        $totalMemory = [math]::Round($os.TotalVisibleMemorySize / 1024 / 1024, 2)
        $freeMemory = [math]::Round($os.FreePhysicalMemory / 1024 / 1024, 2)

        Write-Host "  Total Physical Memory: $totalMemory GB" -ForegroundColor White
        Write-Host "  Free Physical Memory: $freeMemory GB" -ForegroundColor $(if ($freeMemory -gt 1) { 'Green' } else { 'Yellow' })

        $memoryUsagePercent = [math]::Round((1 - ($freeMemory / $totalMemory)) * 100, 1)
        Write-Host "  Memory Usage: $memoryUsagePercent%" -ForegroundColor $(if ($memoryUsagePercent -lt 90) { 'Green' } else { 'Red' })

        return $true
    }
    catch {
        Write-Host "  ERROR: Could not check system resources - $($_.Exception.Message)" -ForegroundColor Red
        return $false
    }
}

function Diagnose-MMCSS-Issues {
    Write-Header "MMCSS Diagnostics"

    $isAdmin = Check-Administrator
    Write-Host "Running as Administrator: $(if ($isAdmin) { 'YES' } else { 'NO - This may cause MMCSS failures' })" -ForegroundColor $(if ($isAdmin) { 'Green' } else { 'Red' })

    Write-Host ""

    $results = @{
        Service = Check-MMCSS-Service
        PageFile = Check-PagingFile
        DLLs = Check-MMCSS-DLLs
        Resources = Check-System-Resources
        Admin = $isAdmin
    }

    Write-Header "Diagnostic Summary"

    Write-Host "MMCSS Service: $(if ($results.Service) { 'OK' } else { 'FAILED' })" -ForegroundColor $(if ($results.Service) { 'Green' } else { 'Red' })
    Write-Host "Page File: $(if ($results.PageFile) { 'OK' } else { 'WARNING' })" -ForegroundColor $(if ($results.PageFile) { 'Green' } else { 'Yellow' })
    Write-Host "MMCSS DLLs: $(if ($results.DLLs) { 'OK' } else { 'FAILED' })" -ForegroundColor $(if ($results.DLLs) { 'Green' } else { 'Red' })
    Write-Host "System Resources: $(if ($results.Resources) { 'OK' } else { 'WARNING' })" -ForegroundColor $(if ($results.Resources) { 'Green' } else { 'Yellow' })
    Write-Host "Administrator Privileges: $(if ($results.Admin) { 'OK' } else { 'REQUIRED' })" -ForegroundColor $(if ($results.Admin) { 'Green' } else { 'Red' })

    $overallStatus = $results.Service -and $results.DLLs -and $results.Admin
    Write-Host ""
    Write-Host "Overall MMCSS Status: $(if ($overallStatus) { 'GOOD' } else { 'ISSUES DETECTED' })" -ForegroundColor $(if ($overallStatus) { 'Green' } else { 'Red' })

    if (-not $overallStatus) {
        Write-Host ""
        Write-Host "Recommendations:" -ForegroundColor Yellow
        if (-not $results.Admin) {
            Write-Host "  - Run this script and your application as Administrator" -ForegroundColor Yellow
        }
        if (-not $results.Service) {
            Write-Host "  - MMCSS service issues detected - check Windows Services" -ForegroundColor Yellow
        }
        if (-not $results.DLLs) {
            Write-Host "  - MMCSS DLLs missing - repair Windows installation" -ForegroundColor Yellow
        }
        if (-not $results.PageFile) {
            Write-Host "  - Page file issues detected - increase virtual memory" -ForegroundColor Yellow
        }
    }

    return $overallStatus
}

function Fix-MMCSS-Issues {
    Write-Header "MMCSS Fixes"

    $isAdmin = Check-Administrator
    if (-not $isAdmin) {
        Write-Host "ERROR: Administrator privileges required to apply fixes!" -ForegroundColor Red
        Write-Host "Please run this script as Administrator." -ForegroundColor Yellow
        return $false
    }

    Write-Host "Applying MMCSS fixes..." -ForegroundColor Green

    # Fix 1: Ensure MMCSS service is running and set to auto-start
    Write-Host "Fix 1: MMCSS Service Configuration" -ForegroundColor Cyan
    try {
        Set-Service -Name MMCSS -StartupType Automatic -ErrorAction Stop
        Start-Service -Name MMCSS -ErrorAction Stop
        Write-Host "  [OK] MMCSS service configured and started" -ForegroundColor Green
    }
    catch {
        Write-Host "  [FAIL] Failed to configure MMCSS service: $($_.Exception.Message)" -ForegroundColor Red
    }

    # Fix 2: Optimize page file (if system managed)
    Write-Host "Fix 2: Page File Optimization" -ForegroundColor Cyan
    try {
        $pageFile = Get-WmiObject -Class Win32_PageFileSetting
        if ($pageFile) {
            # If system managed, ensure it's properly configured
            if ($pageFile.InitialSize -eq 0 -and $pageFile.MaximumSize -eq 0) {
                Write-Host "  [OK] Page file is system managed (recommended)" -ForegroundColor Green
            } else {
                Write-Host "  [INFO] Page file has custom size - consider system managed for better MMCSS compatibility" -ForegroundColor Yellow
            }
        }
    }
    catch {
        Write-Host "  [FAIL] Failed to check page file: $($_.Exception.Message)" -ForegroundColor Yellow
    }

    # Fix 3: Registry optimization for MMCSS
    Write-Host "Fix 3: MMCSS Registry Optimization" -ForegroundColor Cyan
    try {
        $regPath = "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Multimedia\SystemProfile"
        if (Test-Path $regPath) {
            # Ensure MMCSS is enabled
            Set-ItemProperty -Path $regPath -Name "SystemResponsiveness" -Value 20 -ErrorAction Stop
            Write-Host "  [OK] MMCSS responsiveness optimized" -ForegroundColor Green
        } else {
            Write-Host "  [FAIL] MMCSS registry path not found" -ForegroundColor Red
        }
    }
    catch {
        Write-Host "  [FAIL] Failed to optimize MMCSS registry: $($_.Exception.Message)" -ForegroundColor Red
    }

    # Fix 4: Network optimization for gaming
    Write-Host "Fix 4: Network Optimization" -ForegroundColor Cyan
    try {
        $netRegPath = "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Multimedia\SystemProfile\Tasks\Games"
        if (-not (Test-Path $netRegPath)) {
            New-Item -Path $netRegPath -Force | Out-Null
        }
        Set-ItemProperty -Path $netRegPath -Name "Affinity" -Value 0 -ErrorAction Stop
        Set-ItemProperty -Path $netRegPath -Name "Background Only" -Value "False" -ErrorAction Stop
        Set-ItemProperty -Path $netRegPath -Name "Clock Rate" -Value 10000 -ErrorAction Stop
        Set-ItemProperty -Path $netRegPath -Name "GPU Priority" -Value 8 -ErrorAction Stop
        Set-ItemProperty -Path $netRegPath -Name "Priority" -Value 6 -ErrorAction Stop
        Set-ItemProperty -Path $netRegPath -Name "Scheduling Category" -Value "High" -ErrorAction Stop
        Set-ItemProperty -Path $netRegPath -Name "SFIO Priority" -Value "High" -ErrorAction Stop
        Write-Host "  [OK] Network optimization applied" -ForegroundColor Green
    }
    catch {
        Write-Host "  [FAIL] Failed to apply network optimization: $($_.Exception.Message)" -ForegroundColor Yellow
    }

    Write-Host ""
    Write-Host "MMCSS fixes applied. Please restart your application to test the changes." -ForegroundColor Green
    return $true
}

# Main execution
if ($Diagnose -or $All) {
    Diagnose-MMCSS-Issues
}

if ($Fix -or $All) {
    Write-Host ""
    Fix-MMCSS-Issues
}

if (-not $Diagnose -and -not $Fix -and -not $All) {
    Write-Host "MMCSS Diagnostic and Fix Script" -ForegroundColor Cyan
    Write-Host ""
    Write-Host "Usage:" -ForegroundColor White
    Write-Host "  .\fix_mmcss.ps1 -Diagnose    # Run diagnostics only" -ForegroundColor White
    Write-Host "  .\fix_mmcss.ps1 -Fix         # Apply fixes only" -ForegroundColor White
    Write-Host "  .\fix_mmcss.ps1 -All         # Run diagnostics and apply fixes" -ForegroundColor White
    Write-Host ""
    Write-Host "Examples:" -ForegroundColor Yellow
    Write-Host "  .\fix_mmcss.ps1 -Diagnose" -ForegroundColor White
    Write-Host "  .\fix_mmcss.ps1 -All" -ForegroundColor White
}
