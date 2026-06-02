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

Write-Host "--- [1/4] RHI Translate Multi-Queue Test ---" -ForegroundColor Yellow

do {
    $Exe   = Join-Path $BinDir "TestRHITranslate.exe"
    $Label = "RHI"
    if (-not (Build-Target -Target "TestRHITranslate")) {
        Register-Fail $Label "build failed"
        break
    }
    if (-not (Assert-Exe $Exe)) {
        Register-Fail $Label "executable missing"
        break
    }

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
    $requiredCases = @("RHICommandListRGBaseline", "RenderGraphContractFoundation")
    $missingRequiredCases = @()
    if (@($structuredCases).Count -eq 0) {
        Register-Subtest -Group $Label -Name "StructuredTestcaseMarkers" -Status "FAILED" -Reason "no structured testcase markers found"
    } else {
        foreach ($case in $structuredCases) {
            Register-Subtest -Group $Label -Name $case.Name -Status $case.Status -Reason $case.Reason
        }
        foreach ($requiredCase in $requiredCases) {
            if (-not ($structuredCases | Where-Object { $_.Name -eq $requiredCase })) {
                $missingRequiredCases += $requiredCase
                Register-Subtest -Group $Label -Name $requiredCase -Status "FAILED" -Reason "required testcase marker missing"
            }
        }
    }

    Write-Host "[$Label] Output tail:"
    Get-Content $Log -Tail 10 | ForEach-Object { Write-Host "  $_" }

    $structuredFailures = @($structuredCases | Where-Object { $_.Status -eq "FAILED" })

    if ($exitCode -eq 0 -and
        @($errorLines).Count -eq 0 -and
        $capability.State -ne "Unknown" -and
        @($structuredCases).Count -gt 0 -and
        @($structuredFailures).Count -eq 0 -and
        @($missingRequiredCases).Count -eq 0) {
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
        } elseif (@($missingRequiredCases).Count -gt 0) {
            "missing required testcase: $($missingRequiredCases -join ', ')"
        } else {
            "validation blockers detected"
        }
        Register-Fail $Label $reason
    }
} while ($false)

Write-Host ""

Write-Host "--- [2/4] TaskGraph / TaskPipe Regression ---" -ForegroundColor Yellow
Invoke-StructuredBinaryTest `
    -Label "TaskGraph" `
    -Target "TestTaskGraph" `
    -ExePath (Join-Path $BinDir "TestTaskGraph.exe") `
    -WorkDir $BinDir `
    -LogFile (Join-Path $script:RunDir "taskgraph.log") `
    -ExtraArgs $ExtraArgs
Invoke-StructuredBinaryTest `
    -Label "TaskPipe" `
    -Target "TestTaskPipe" `
    -ExePath (Join-Path $BinDir "TestTaskPipe.exe") `
    -WorkDir $BinDir `
    -LogFile (Join-Path $script:RunDir "taskpipe.log") `
    -ExtraArgs $ExtraArgs

Write-Host "--- [3/4] MoerEditor Test (20 s) ---" -ForegroundColor Yellow

do {
    $Exe      = Join-Path $BinDir "MoerEditor.exe"
    $Label    = "Editor"
    if (-not (Build-Target -Target "MoerEditor")) {
        Register-Fail $Label "build failed"
        break
    }
    if (-not (Assert-Exe $Exe)) {
        Register-Fail $Label "executable missing"
        break
    }

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
    $descriptorHeapCapability = Get-DescriptorHeapCapabilityState -LogFile $Log
    $runtimeUnsupported = $descriptorHeapCapability.State -eq "Skipped"

    if ($errorLines) {
        Write-Host "[$Label] Errors/validation issues detected:" -ForegroundColor Red
        $errorLines | ForEach-Object { Write-Host "  $($_.Line)" }
        $errorLines | ForEach-Object { $_.Line } | Out-File $CrashLog -Encoding UTF8
        Write-Summary "[$Label] Errors/validation issues detected ($(@($errorLines).Count) lines)."
        $failed = $true
    }
    Save-Minidumps -BinDir $BinDir

    if ($runtimeUnsupported -and -not $errorLines) {
        Register-Skip $Label $descriptorHeapCapability.Reason
    } elseif (-not $failed) {
        Register-Pass $Label
    } else {
        Register-Fail $Label "see $Log"
    }
} while ($false)

Write-Host ""

Write-Host "--- [4/4] MoerProfiler Test (15 s) ---" -ForegroundColor Yellow

do {
    $Exe      = Join-Path $BinDir "MoerProfiler.exe"
    $Label    = "Profiler"
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
    Write-Host "[$Label] Will kill after 15 s..."

    $RunStart = Get-Date
    $survived = Invoke-ExeTimed -ExePath $Exe -WorkDir $BinDir -LogFile $Log `
                                -ExtraArgs $ExtraArgs -TimeoutSec 15
    Merge-Stderr $Log "$Log.stderr.tmp"

    $failed = $false
    if ($survived) {
        Write-Host "[$Label] Killed after 15 s."
    } else {
        Write-Host "[$Label] Process exited early (code: $($script:TimedExitCode))." -ForegroundColor DarkYellow
        Write-Summary "[$Label] Process exited early (code: $($script:TimedExitCode))."
        if ($script:TimedExitCode -ne 0) { $failed = $true }
    }

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

    $errorLines = Test-LogForErrors -LogFile $Log
    if (@($errorLines).Count -gt 0) {
        Write-Host "[$Label] Vulkan validation errors / crashes detected:" -ForegroundColor Red
        $errorLines | ForEach-Object { Write-Host "  $($_.Line)" }
        $errorLines | ForEach-Object { $_.Line } | Out-File $CrashLog -Encoding UTF8
        Write-Summary "[$Label] Vulkan validation errors / crashes detected ($(@($errorLines).Count) lines)."
        $failed = $true
    }
    Save-Minidumps -BinDir $BinDir

    if (-not $failed) {
        Register-Pass $Label
    } else {
        Register-Fail $Label "see $Log"
    }

    Write-Host "[$Label] Output tail:"
    Get-Content $Log -Tail 10 | ForEach-Object { Write-Host "  $_" }
} while ($false)

Write-Host ""
Finish-TestRun