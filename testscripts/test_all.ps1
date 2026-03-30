#Requires -Version 5.1
<#
.SYNOPSIS
    Run all MoerEngine self-check tests in sequence.

.DESCRIPTION
    Aggregates results from all individual test scripts into one summary.
    Each test writes to its own log file inside the shared run directory.

.PARAMETER Config
    Build configuration: Debug (default), Release, RelWithDebInfo

.PARAMETER ExtraArgs
    Extra arguments forwarded to every test executable.

.EXAMPLE
    .\test_all.ps1
    .\test_all.ps1 -Config Release
    .\test_all.ps1 -ExtraArgs @("-DENABLE_VALIDATION=1")
#>
param(
    [ValidateSet("Debug","Release","RelWithDebInfo")]
    [string]$Config = "Debug",

    [string[]]$ExtraArgs = @()
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. "$PSScriptRoot\common.ps1"

Initialize-TestRun -Config $Config -ScriptDir $PSScriptRoot

$BinDir = Join-Path $script:Root "target\bin\$Config"

# ─── 1. RHI Translate Multi-Queue ────────────────────────────────────────────
Write-Host "--- [1/2] RHI Translate Multi-Queue Test ---" -ForegroundColor Yellow

do {
    $Exe   = Join-Path $BinDir "TestRHITranslate.exe"
    $Label = "RHI"
    if (-not (Assert-Exe $Exe)) { break }

    $Log      = Join-Path $script:RunDir "rhi_translate.log"
    Write-Host "[$Label] Exe : $Exe"
    Write-Host "[$Label] Log : $Log"
    if ($ExtraArgs.Count -gt 0) { Write-Host "[$Label] Args: $($ExtraArgs -join ' ')" }

    $exitCode = Invoke-ExeSync -ExePath $Exe -WorkDir $BinDir -LogFile $Log -ExtraArgs $ExtraArgs

    Write-Host "[$Label] Output tail:"
    Get-Content $Log -Tail 10 | ForEach-Object { Write-Host "  $_" }

    if ($exitCode -eq 0) {
        Register-Pass $Label
    } else {
        Select-String -Path $Log -Pattern "mismatch|failed|\[error\]" -CaseSensitive:$false |
            ForEach-Object { Write-Host "  $($_.Line)" }
        Register-Fail $Label "exit $exitCode"
    }
} while ($false)

Write-Host ""

# ─── 2. MoerEditor (20 s) ────────────────────────────────────────────────────
Write-Host "--- [2/2] MoerEditor Test (20 s) ---" -ForegroundColor Yellow

do {
    $Exe      = Join-Path $BinDir "MoerEditor.exe"
    $Label    = "Editor"
    if (-not (Assert-Exe $Exe)) { break }

    $Log      = Join-Path $script:RunDir "moereditor.log"
    $CrashLog = Join-Path $script:RunDir "moereditor_crash.txt"
    Write-Host "[$Label] Exe : $Exe"
    Write-Host "[$Label] Log : $Log"
    Write-Host "[$Label] Will kill after 20 s..."

    $survived = Invoke-ExeTimed -ExePath $Exe -WorkDir $BinDir -LogFile $Log `
                                -ExtraArgs $ExtraArgs -TimeoutSec 20
    Merge-Stderr $Log "$Log.stderr.tmp"

    $failed = $false
    if ($survived) {
        Write-Host "[$Label] Killed after 20 s."
    } else {
        Write-Host "[$Label] Process exited early (code: $($script:TimedExitCode))." -ForegroundColor DarkYellow
        Write-Summary "[$Label] Process exited early (code: $($script:TimedExitCode))."
        if ($script:TimedExitCode -ne 0) { $failed = $true }
    }

    $errorLines = Test-LogForErrors -LogFile $Log
    if ($errorLines) {
        Write-Host "[$Label] Errors/validation issues detected:" -ForegroundColor Red
        $errorLines | ForEach-Object { Write-Host "  $($_.Line)" }
        $errorLines | ForEach-Object { $_.Line } | Out-File $CrashLog -Encoding UTF8
        Write-Summary "[$Label] Errors/validation issues detected ($($errorLines.Count) lines)."
        $failed = $true
    }
    Save-Minidumps -BinDir $BinDir

    if (-not $failed) { Register-Pass $Label } else { Register-Fail $Label "see $Log" }
} while ($false)

Write-Host ""
Finish-TestRun
