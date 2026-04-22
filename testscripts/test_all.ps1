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
Write-Host "--- [1/3] RHI Translate Multi-Queue Test ---" -ForegroundColor Yellow

do {
    $Exe   = Join-Path $BinDir "TestRHITranslate.exe"
    $Label = "RHI"
    if (-not (Build-Target -Target "TestRHITranslate")) { break }
    if (-not (Assert-Exe $Exe)) { break }

    $Log      = Join-Path $script:RunDir "rhi_translate.log"
    Write-Host "[$Label] Exe : $Exe"
    Write-Host "[$Label] Log : $Log"
    if ($ExtraArgs.Count -gt 0) { Write-Host "[$Label] Args: $($ExtraArgs -join ' ')" }

    $exitCode = Invoke-ExeSync -ExePath $Exe -WorkDir $BinDir -LogFile $Log -ExtraArgs $ExtraArgs

    $capability = Get-DescriptorHeapCapabilityState -LogFile $Log
    switch ($capability.State) {
        "Enabled" {
            Register-Subtest -Group $Label -Name "DescriptorHeapCapability" -Status "PASSED" -Reason $capability.Reason
        }
        "Skipped" {
            Register-Subtest -Group $Label -Name "DescriptorHeapCapability" -Status "SKIPPED" -Reason $capability.Reason
        }
        default {
            Register-Subtest -Group $Label -Name "DescriptorHeapCapability" -Status "FAILED" -Reason $capability.Reason
        }
    }

    $errorLines = Test-LogForErrors -LogFile $Log
    if (@($errorLines).Count -gt 0) {
        Register-Subtest -Group $Label -Name "ValidationBlockers" -Status "FAILED" -Reason "$(@($errorLines).Count) blocking lines"
    } else {
        Register-Subtest -Group $Label -Name "ValidationBlockers" -Status "PASSED"
    }

    $structuredCases = Get-StructuredTestCaseResults -LogFile $Log
    if (@($structuredCases).Count -eq 0) {
        Register-Subtest -Group $Label -Name "StructuredTestcaseMarkers" -Status "FAILED" -Reason "no structured testcase markers found"
    } else {
        foreach ($case in $structuredCases) {
            Register-Subtest -Group $Label -Name $case.Name -Status $case.Status -Reason $case.Reason
        }
    }

    Write-Host "[$Label] Output tail:"
    Get-Content $Log -Tail 10 | ForEach-Object { Write-Host "  $_" }

    $structuredFailures = @($structuredCases | Where-Object { $_.Status -eq "FAILED" })

    if ($exitCode -eq 0 -and
        @($errorLines).Count -eq 0 -and
        $capability.State -ne "Unknown" -and
        @($structuredCases).Count -gt 0 -and
        @($structuredFailures).Count -eq 0) {
        Register-Pass $Label
    } else {
        Select-String -Path $Log -Pattern "mismatch|failed|\[error\]" -CaseSensitive:$false |
            ForEach-Object { Write-Host "  $($_.Line)" }
        if (@($errorLines).Count -gt 0) {
            $errorLines | ForEach-Object { Write-Host "  $($_.Line)" }
        }
        $reason = if ($exitCode -ne 0) {
            "exit $exitCode"
        } elseif ($capability.State -eq "Unknown") {
            "descriptor heap capability state missing"
        } elseif (@($structuredCases).Count -eq 0) {
            "structured testcase markers missing"
        } elseif (@($structuredFailures).Count -gt 0) {
            "$(@($structuredFailures).Count) structured testcase failures"
        } else {
            "validation blockers detected"
        }
        Register-Fail $Label $reason
    }
} while ($false)

Write-Host ""

# ─── 2. TaskGraph / TaskPipe Regression ──────────────────────────────────────
function Invoke-StructuredBinaryTest {
    param(
        [Parameter(Mandatory)][string]$Label,
        [Parameter(Mandatory)][string]$Target,
        [Parameter(Mandatory)][string]$ExeName,
        [Parameter(Mandatory)][string]$LogName
    )

    $Exe = Join-Path $BinDir $ExeName
    $Log = Join-Path $script:RunDir $LogName

    do {
        if (-not (Build-Target -Target $Target)) { break }
        if (-not (Assert-Exe $Exe)) { break }

        Write-Host "[$Label] Exe : $Exe"
        Write-Host "[$Label] Log : $Log"
        if ($ExtraArgs.Count -gt 0) { Write-Host "[$Label] Args: $($ExtraArgs -join ' ')" }

        $exitCode = Invoke-ExeSync -ExePath $Exe -WorkDir $BinDir -LogFile $Log -ExtraArgs $ExtraArgs
        $errorLines = Test-LogForErrors -LogFile $Log
        $structuredCases = Get-StructuredTestCaseResults -LogFile $Log

        if (@($structuredCases).Count -eq 0) {
            Register-Subtest -Group $Label -Name "StructuredTestcaseMarkers" -Status "FAILED" -Reason "no structured testcase markers found"
        } else {
            foreach ($case in $structuredCases) {
                Register-Subtest -Group $Label -Name $case.Name -Status $case.Status -Reason $case.Reason
            }
        }

        if (@($errorLines).Count -gt 0) {
            Register-Subtest -Group $Label -Name "ValidationBlockers" -Status "FAILED" -Reason "$(@($errorLines).Count) blocking lines"
        } else {
            Register-Subtest -Group $Label -Name "ValidationBlockers" -Status "PASSED"
        }

        Write-Host "[$Label] Output tail:"
        Get-Content $Log -Tail 10 | ForEach-Object { Write-Host "  $_" }

        $structuredFailures = @($structuredCases | Where-Object { $_.Status -eq "FAILED" })
        if ($exitCode -eq 0 -and @($structuredCases).Count -gt 0 -and @($structuredFailures).Count -eq 0 -and @($errorLines).Count -eq 0) {
            Register-Pass $Label
        } else {
            $reason = if ($exitCode -ne 0) {
                "exit $exitCode"
            } elseif (@($structuredCases).Count -eq 0) {
                "structured testcase markers missing"
            } elseif (@($structuredFailures).Count -gt 0) {
                "$(@($structuredFailures).Count) structured testcase failures"
            } else {
                "validation blockers detected"
            }
            Register-Fail $Label $reason
        }
    } while ($false)

    Write-Host ""
}

Write-Host "--- [2/3] TaskGraph / TaskPipe Regression ---" -ForegroundColor Yellow
Invoke-StructuredBinaryTest -Label "TaskGraph" -Target "TestTaskGraph" -ExeName "TestTaskGraph.exe" -LogName "taskgraph.log"
Invoke-StructuredBinaryTest -Label "TaskPipe" -Target "TestTaskPipe" -ExeName "TestTaskPipe.exe" -LogName "taskpipe.log"

# ─── 3. MoerEditor (20 s) ────────────────────────────────────────────────────
Write-Host "--- [3/3] MoerEditor Test (20 s) ---" -ForegroundColor Yellow

do {
    $Exe      = Join-Path $BinDir "MoerEditor.exe"
    $Label    = "Editor"
    if (-not (Build-Target -Target "MoerEditor")) { break }
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
        Write-Summary "[$Label] Errors/validation issues detected ($(@($errorLines).Count) lines)."
        $failed = $true
    }
    Save-Minidumps -BinDir $BinDir

    if (-not $failed) { Register-Pass $Label } else { Register-Fail $Label "see $Log" }
} while ($false)

Write-Host ""
Finish-TestRun
