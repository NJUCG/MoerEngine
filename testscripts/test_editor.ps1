#Requires -Version 5.1
<#
.SYNOPSIS
    Run MoerEditor.exe for 20 seconds, then terminate and inspect the log.

.PARAMETER Config
    Build configuration: Debug (default), Release, RelWithDebInfo

.PARAMETER TimeoutSec
    How many seconds to let MoerEditor run before killing it (default: 20).

.PARAMETER ExtraArgs
    Extra arguments forwarded to MoerEditor.exe.

.EXAMPLE
    .\test_editor.ps1
    .\test_editor.ps1 -Config Release
    .\test_editor.ps1 -TimeoutSec 30
#>
param(
    [ValidateSet("Debug","Release","RelWithDebInfo")]
    [string]$Config = "Debug",

    [int]$TimeoutSec = 20,

    [string[]]$ExtraArgs = @()
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. "$PSScriptRoot\common.ps1"

Initialize-TestRun -Config $Config -ScriptDir $PSScriptRoot

$BinDir = Join-Path $script:Root "target\bin\$Config"
$Exe    = Join-Path $BinDir "MoerEditor.exe"
$Label  = "Editor"

Write-Host "--- MoerEditor Test ($TimeoutSec s) ---" -ForegroundColor Yellow

do {
    if (-not (Build-Target -Target "MoerEditor")) { break }
    if (-not (Assert-Exe $Exe)) { break }

    $Log      = Join-Path $script:RunDir "moereditor.log"
    $CrashLog = Join-Path $script:RunDir "moereditor_crash.txt"

    Write-Host "[$Label] Exe : $Exe"
    Write-Host "[$Label] Log : $Log"
    Write-Host "[$Label] Will kill after $TimeoutSec s..."

    $survived = Invoke-ExeTimed -ExePath $Exe -WorkDir $BinDir -LogFile $Log `
                                -ExtraArgs $ExtraArgs -TimeoutSec $TimeoutSec
    Merge-Stderr $Log "$Log.stderr.tmp"

    $failed = $false

    if ($survived) {
        Write-Host "[$Label] Killed after $TimeoutSec s."
    } else {
        Write-Host "[$Label] Process exited early (code: $($script:TimedExitCode))." -ForegroundColor DarkYellow
        Write-Summary "[$Label] Process exited early (code: $($script:TimedExitCode))."
        if ($script:TimedExitCode -ne 0) { $failed = $true }
    }

    # Crash + Vulkan validation error scan
    $errorLines = Test-LogForErrors -LogFile $Log
    if ($errorLines) {
        Write-Host "[$Label] Errors/validation issues detected:" -ForegroundColor Red
        $errorLines | ForEach-Object { Write-Host "  $($_.Line)" }
        $errorLines | ForEach-Object { $_.Line } | Out-File $CrashLog -Encoding UTF8
        Write-Summary "[$Label] Errors/validation issues detected ($(@($errorLines).Count) lines)."
        $failed = $true
    }

    Save-Minidumps -BinDir $BinDir

    if (-not $failed) {
        Register-Pass $Label
    } else {
        Register-Fail $Label "see $Log"
    }
} while ($false)

Write-Host ""
Finish-TestRun
