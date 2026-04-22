#Requires -Version 5.1
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

Write-Host "--- [1/2] TaskGraph Regression ---" -ForegroundColor Yellow
Invoke-StructuredBinaryTest -Label "TaskGraph" -Target "TestTaskGraph" -ExeName "TestTaskGraph.exe" -LogName "taskgraph.log"

Write-Host "--- [2/2] TaskPipe Regression ---" -ForegroundColor Yellow
Invoke-StructuredBinaryTest -Label "TaskPipe" -Target "TestTaskPipe" -ExeName "TestTaskPipe.exe" -LogName "taskpipe.log"

Finish-TestRun
