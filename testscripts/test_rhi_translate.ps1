#Requires -Version 5.1
<#
.SYNOPSIS
    Run TestRHITranslate.exe (RHI multi-queue translate test).

.PARAMETER Config
    Build configuration: Debug (default), Release, RelWithDebInfo

.PARAMETER ExtraArgs
    Extra arguments forwarded to the executable.

.EXAMPLE
    .\test_rhi_translate.ps1
    .\test_rhi_translate.ps1 -Config Release
    .\test_rhi_translate.ps1 -ExtraArgs @("-DENABLE_VALIDATION=1")
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
$Exe    = Join-Path $BinDir "TestRHITranslate.exe"
$Label  = "RHI"

Write-Host "--- RHI Translate Multi-Queue Test ---" -ForegroundColor Yellow

do {
    if (-not (Build-Target -Target "TestRHITranslate")) { break }
    if (-not (Assert-Exe $Exe)) { break }

    $Log = Join-Path $script:RunDir "rhi_translate.log"

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

    # Print tail for quick feedback
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
        # Surface failure lines in console and summary
        $fails = Select-String -Path $Log -Pattern "mismatch|failed|\[error\]" -CaseSensitive:$false
        $fails | ForEach-Object { Write-Host "  $($_.Line)" }
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
Finish-TestRun
