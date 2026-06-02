#Requires -Version 5.1
<#
.SYNOPSIS
    Smoke-test MoerProfiler for Vulkan validation errors.

.DESCRIPTION
    Builds MoerProfiler, launches it for a short window, then kills it and
    scans the log for validation errors, crashes, or early exits.

.PARAMETER Config
    Build configuration: Debug, Release, or RelWithDebInfo.
    Debug builds enable the Vulkan validation layer — this is the default and
    the recommended way to catch validation errors.

.PARAMETER TimeoutSec
    How long to let the profiler run before killing it.
    Default 15 s is enough to catch startup/shutdown validation errors.

.PARAMETER CaptureFile
    Optional path to a profiler capture file (.mpd, .mrtc, .csv, .bin).
    When supplied the profiler loads this file on startup, which exercises
    the capture loading code path as well.

.PARAMETER ExtraArgs
    Additional arguments forwarded to MoerProfiler.exe.
#>

param(
    [ValidateSet("Debug","Release","RelWithDebInfo")]
    [string]$Config = "Debug",

    [int]$TimeoutSec = 15,

    [string]$CaptureFile = "",

    [string[]]$ExtraArgs = @()
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. "$PSScriptRoot\common.ps1"

Initialize-TestRun -Config $Config -ScriptDir $PSScriptRoot

$BinDir = Join-Path $script:Root "target\bin\$Config"
$Exe    = Join-Path $BinDir "MoerProfiler.exe"
$Label  = "Profiler"

# ---------------------------------------------------------------
# Build arg list — optional capture file comes first
# ---------------------------------------------------------------
$AllArgs = [System.Collections.ArrayList]::new()
if ($CaptureFile) {
    [void]$AllArgs.Add($CaptureFile)
}
if ($ExtraArgs.Count -gt 0) {
    [void]$AllArgs.AddRange($ExtraArgs)
}

$Description = if ($CaptureFile) { "Profiler (capture: $CaptureFile)" } else { "Profiler (empty session)" }
Write-Host "--- $Description ($TimeoutSec s) ---" -ForegroundColor Yellow

do {
    # ---- Build -------------------------------------------------
    if (-not (Build-Target -Target "MoerProfiler")) {
        Register-Fail $Label "build failed"
        break
    }
    if (-not (Assert-Exe $Exe)) {
        Register-Fail $Label "executable missing"
        break
    }

    $Log      = Join-Path $script:RunDir "moerprofiler.log"
    $CrashLog = Join-Path $script:RunDir "moerprofiler_crash.txt"

    Write-Host "[$Label] Exe : $Exe"
    Write-Host "[$Label] Log : $Log"
    if ($AllArgs.Count -gt 0) { Write-Host "[$Label] Args: $($AllArgs -join ' ')" }
    Write-Host "[$Label] Will kill after $TimeoutSec s..."

    # ---- Run ---------------------------------------------------
    $RunStart = Get-Date
    $survived = Invoke-ExeTimed -ExePath $Exe -WorkDir $BinDir -LogFile $Log `
                                -ExtraArgs $AllArgs.ToArray() -TimeoutSec $TimeoutSec
    Merge-Stderr $Log "$Log.stderr.tmp"

    $failed = $false

    # ---- Interpret exit / kill --------------------------------
    if ($survived) {
        Write-Host "[$Label] Killed after $TimeoutSec s (expected for GUI app)."
    } else {
        Write-Host "[$Label] Process exited early (code: $($script:TimedExitCode))." -ForegroundColor DarkYellow
        Write-Summary "[$Label] Process exited early (code: $($script:TimedExitCode))."
        if ($script:TimedExitCode -ne 0) {
            $failed = $true
        }
    }

    # ---- Windows Error Reporting ------------------------------
    $werEvents = @(Get-WinEvent -FilterHashtable @{LogName='Application'; StartTime=$RunStart} -ErrorAction SilentlyContinue |
        Where-Object {
            ($_.ProviderName -eq 'Application Error' -or $_.ProviderName -eq 'Windows Error Reporting') -and
            $_.Message -match [regex]::Escape((Split-Path $Exe -Leaf))
        })
    if ($werEvents.Count -gt 0) {
        Write-Host "[$Label] Windows crash reports detected:" -ForegroundColor Red
        $werEvents | Select-Object -First 4 | ForEach-Object {
            Write-Host "  $($_.TimeCreated) $($_.ProviderName) event $($_.Id)"
        }
        $werEvents | Select-Object TimeCreated,ProviderName,Id,Message |
            Format-List | Out-File $CrashLog -Encoding UTF8 -Append
        Write-Summary "[$Label] Windows crash reports detected ($($werEvents.Count) events)."
        $failed = $true
    }

    # ---- Validation errors / crashes in log -------------------
    $errorLines = Test-LogForErrors -LogFile $Log
    if (@($errorLines).Count -gt 0) {
        Write-Host "[$Label] Vulkan validation errors / crashes detected:" -ForegroundColor Red
        $errorLines | ForEach-Object { Write-Host "  $($_.Line)" }
        $errorLines | ForEach-Object { $_.Line } | Out-File $CrashLog -Encoding UTF8
        Write-Summary "[$Label] Vulkan validation errors / crashes detected ($(@($errorLines).Count) lines)."

        # ---- Categorize by VUID ---------------------------------
        $vuids = @{}
        foreach ($line in $errorLines) {
            if ($line.Line -match '\[VUID-([^\]]+)\]') {
                $vuid = $matches[1]
                if (-not $vuids.ContainsKey($vuid)) { $vuids[$vuid] = 0 }
                $vuids[$vuid]++
            }
        }
        if ($vuids.Count -gt 0) {
            Write-Host "[$Label] Unique VUIDs:" -ForegroundColor Red
            foreach ($entry in $vuids.GetEnumerator() | Sort-Object -Property Value -Descending) {
                Write-Host "    VUID-$($entry.Key)  (x$($entry.Value))"
            }
            Write-Summary "[$Label] Unique VUIDs: $($vuids.Count)"
        }
        $failed = $true
    } else {
        Write-Host "[$Label] No validation errors detected." -ForegroundColor Green
        Register-Subtest -Group $Label -Name "ValidationBlockers" -Status "PASSED"
    }

    # ---- Check for startup log markers ------------------------
    $started = Select-String -Path $Log -Pattern "MoerProfiler starting" -CaseSensitive:$false
    if ($started) {
        Register-Subtest -Group $Label -Name "Startup" -Status "PASSED"
    } else {
        Register-Subtest -Group $Label -Name "Startup" -Status "FAILED" -Reason "startup log marker missing"
        $failed = $true
    }

    # ---- Check for ingest server ------------------------------
    $ingestStarted = Select-String -Path $Log -Pattern "ProfilerIngest.*(start|listen|bind|19090)" -CaseSensitive:$false
    if ($ingestStarted) {
        Register-Subtest -Group $Label -Name "IngestServer" -Status "PASSED"
    } else {
        # Not necessarily a failure — the log may not print this line.
        # We still flag it as a subtest so the user can investigate.
        Register-Subtest -Group $Label -Name "IngestServer" -Status "SKIPPED" -Reason "no ingest start marker in log"
    }

    # ---- Check shutdown is clean ------------------------------
    $cleanShutdown = Select-String -Path $Log -Pattern "(shut\s*down|shutdown|dispose|destroy).*success" -CaseSensitive:$false
    # Shutdown only happens if the process wasn't killed, so skip when survived.
    if (-not $survived -and $script:TimedExitCode -eq 0 -and $cleanShutdown) {
        Register-Subtest -Group $Label -Name "Shutdown" -Status "PASSED"
    } elseif (-not $survived -and $script:TimedExitCode -eq 0) {
        Register-Subtest -Group $Label -Name "Shutdown" -Status "SKIPPED" -Reason "no explicit shutdown marker"
    } elseif ($survived) {
        Register-Subtest -Group $Label -Name "Shutdown" -Status "SKIPPED" -Reason "process killed — shutdown not exercised"
    }

    # ---- Save any minidumps -----------------------------------
    Save-Minidumps -BinDir $BinDir

    # ---- Final verdict ----------------------------------------
    if (-not $failed) {
        Register-Pass $Label
    } else {
        Register-Fail $Label "see $Log"
    }

    # ---- Log tail for quick inspection ------------------------
    Write-Host "[$Label] Output tail:"
    Get-Content $Log -Tail 15 | ForEach-Object { Write-Host "  $_" }

} while ($false)

Write-Host ""
Finish-TestRun
