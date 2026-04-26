#Requires -Version 5.1
<#
.SYNOPSIS
    Shared utilities for MoerEngine Windows test scripts.
    Dot-source this file from each individual test script.

.DESCRIPTION
    Provides:
      - Initialize-TestRun   : set up log directory and summary file
      - Build-Target         : build target before running tests
      - Assert-Exe           : verify executable exists
      - Merge-Stderr         : fold stderr file into main log
      - Invoke-ExeSync       : run an exe synchronously, capture output, return exit code
      - Invoke-ExeTimed      : run an exe, kill after N seconds, return $true if survived
      - Write-Summary        : append a line to summary.txt
    - Test-LogForErrors    : scan log for crashes AND Vulkan validation errors
    - Get-DescriptorHeapCapabilityState : classify descriptor-heap capability from runtime log
        - Get-StructuredTestCaseResults : parse standardized [TESTCASE][PASS|FAIL|SKIP] markers
      - Save-Minidumps       : copy *.dmp / *.mdmp into run dir
    - Register-Pass/Fail/Skip : record test result
    - Register-Subtest     : record grouped subtest result without affecting top-level exit code
      - Finish-TestRun       : print final PASSED/FAILED banner and exit

    Each script must call Initialize-TestRun before anything else.
    All shared state lives in script-scope variables set by Initialize-TestRun.
#>

$script:RunDir      = $null
$script:SummaryFile = $null
$script:PassCount   = 0
$script:FailCount   = 0
$script:SkipCount   = 0
$script:SubPassCount = 0
$script:SubFailCount = 0
$script:SubSkipCount = 0
$script:Config      = "Debug"
$script:BuildDir    = $null
$script:Root        = $null

function Initialize-TestRun {
    param(
        [string]$Config    = "Debug",
        [string]$ScriptDir = $PSScriptRoot
    )

    $script:Config = $Config
    $script:Root   = Split-Path (Split-Path $ScriptDir -Parent) -Parent

    $LogsRoot = Join-Path $script:Root "logs"
    $Stamp    = Get-Date -Format "yyyyMMdd_HHmmss"
    $script:RunDir      = Join-Path $LogsRoot "run_$Stamp"
    $script:SummaryFile = Join-Path $script:RunDir "summary.txt"
    $script:PassCount   = 0
    $script:FailCount   = 0
    $script:SkipCount   = 0
    $script:SubPassCount = 0
    $script:SubFailCount = 0
    $script:SubSkipCount = 0

    New-Item -ItemType Directory -Path $script:RunDir -Force | Out-Null

    Write-Host ""
    Write-Host "[MoerEngine Test]  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')" -ForegroundColor Cyan
    Write-Host "Config   : $Config"
    Write-Host "Logs     : $($script:RunDir)"
    Write-Host ""
}

function Write-Summary([string]$Line) {
    Add-Content -Path $script:SummaryFile -Value $Line -Encoding UTF8
}

function Resolve-BuildDir {
    $rootBuildDir = Join-Path $script:Root "build"
    if (-not (Test-Path $rootBuildDir)) {
        return $null
    }

    $rootCache = Join-Path $rootBuildDir "CMakeCache.txt"
    if (Test-Path $rootCache) {
        return $rootBuildDir
    }

    $candidates = Get-ChildItem -Path $rootBuildDir -Directory -ErrorAction SilentlyContinue |
        Where-Object { Test-Path (Join-Path $_.FullName "CMakeCache.txt") }

    if (-not $candidates) {
        return $null
    }

    $configToken = switch ($script:Config.ToLowerInvariant()) {
        "debug" { "debug" }
        "release" { "release" }
        "relwithdebinfo" { "relwithdebinfo" }
        default { $script:Config.ToLowerInvariant() }
    }

    $preferred = $candidates | Where-Object { $_.Name.ToLowerInvariant().Contains($configToken) } | Select-Object -First 1
    if ($preferred) {
        return $preferred.FullName
    }

    $vscodeDir = $candidates | Where-Object { $_.Name -eq "vscode" } | Select-Object -First 1
    if ($vscodeDir) {
        return $vscodeDir.FullName
    }

    return ($candidates | Select-Object -First 1).FullName
}

function Write-ResultLine(
    [Parameter(Mandatory)][string]$Scope,
    [Parameter(Mandatory)][string]$Label,
    [Parameter(Mandatory)][ValidateSet("PASSED","FAILED","SKIPPED")][string]$Status,
    [string]$Reason = ""
) {
    $msg = if ($Reason) { "[$Scope][$Label] $Status  ($Reason)" } else { "[$Scope][$Label] $Status" }
    $color = switch ($Status) {
        "PASSED" { "Green" }
        "FAILED" { "Red" }
        default { "DarkYellow" }
    }
    Write-Host $msg -ForegroundColor $color
    Write-Summary $msg
}

function Build-Target {
    param(
        [Parameter(Mandatory)][string]$Target
    )

    if (-not $script:BuildDir) {
        $script:BuildDir = Resolve-BuildDir
    }

    $BuildDir = $script:BuildDir
    if (-not (Test-Path $BuildDir)) {
        Write-Host "[ERROR] Build directory not found: $BuildDir" -ForegroundColor Red
        Write-Summary "[ERROR] Build directory not found: $BuildDir"
        $script:FailCount++
        return $false
    }

    Write-Host "[Build] cmake --build $BuildDir --config $($script:Config) --target $Target"
    & cmake --build $BuildDir --config $script:Config --target $Target | Out-Host
    $buildExitCode = $LASTEXITCODE
    if ($null -eq $buildExitCode) {
        $buildExitCode = 0
    }
    if ($buildExitCode -ne 0) {
        Write-Host "[ERROR] Build failed for target: $Target" -ForegroundColor Red
        Write-Summary "[ERROR] Build failed for target: $Target"
        $script:FailCount++
        return $false
    }
    return $true
}

function Assert-Exe([string]$ExePath) {
    if (-not (Test-Path $ExePath)) {
        Write-Host "[ERROR] Not found: $ExePath" -ForegroundColor Red
        Write-Host "        Build first: cmake --build build --config $($script:Config)"
        Write-Summary "[ERROR] Not found: $ExePath"
        $script:FailCount++
        return $false
    }
    return $true
}

function Merge-Stderr([string]$MainLog, [string]$ErrLog) {
    if (Test-Path $ErrLog) {
        if ((Get-Item $ErrLog).Length -gt 0) {
            Add-Content $MainLog "`n--- stderr ---"
            Get-Content $ErrLog | Add-Content $MainLog
        }
        Remove-Item $ErrLog -ErrorAction SilentlyContinue
    }
}

function Stop-ProcessTree {
    param(
        [Parameter(Mandatory)][int]$ProcessId,
        [int]$GraceSec = 10
    )

    $proc = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
    if ($null -ne $proc) {
        $proc.Refresh()
        if ($proc.MainWindowHandle -ne 0) {
            [void]$proc.CloseMainWindow()
            if ($proc.WaitForExit($GraceSec * 1000)) {
                return $true
            }
        }
    }

    $taskkill = Start-Process -FilePath "taskkill.exe" -ArgumentList @("/PID", "$ProcessId", "/T", "/F") -NoNewWindow -PassThru -Wait
    return $taskkill.ExitCode -eq 0 -or $taskkill.ExitCode -eq 128 -or $taskkill.ExitCode -eq 255
}

function Test-ProcessExited {
    param([Parameter(Mandatory)][int]$ProcessId)

    return $null -eq (Get-Process -Id $ProcessId -ErrorAction SilentlyContinue)
}

function Invoke-ExeSync {
    param(
        [Parameter(Mandatory)][string]   $ExePath,
        [Parameter(Mandatory)][string]   $WorkDir,
        [Parameter(Mandatory)][string]   $LogFile,
        [string]  $ErrFile  = "$LogFile.stderr.tmp",
        [string[]]$ExtraArgs = @()
    )

    $p = @{
        FilePath               = $ExePath
        WorkingDirectory       = $WorkDir
        RedirectStandardOutput = $LogFile
        RedirectStandardError  = $ErrFile
        PassThru               = $true
        NoNewWindow            = $true
        Wait                   = $true
    }
    if ($ExtraArgs.Count -gt 0) { $p['ArgumentList'] = $ExtraArgs }

    $proc = Start-Process @p
    Merge-Stderr $LogFile $ErrFile
    return $proc.ExitCode
}

function Invoke-StructuredBinaryTest {
    param(
        [Parameter(Mandatory)][string]$Label,
        [Parameter(Mandatory)][string]$Target,
        [Parameter(Mandatory)][string]$ExePath,
        [Parameter(Mandatory)][string]$WorkDir,
        [Parameter(Mandatory)][string]$LogFile,
        [string[]]$ExtraArgs = @()
    )

    $missingMarkerReason = "structured testcase markers missing"

    do {
        if (-not (Build-Target -Target $Target)) {
            Register-Fail $Label "build failed"
            break
        }
        if (-not (Assert-Exe $ExePath)) {
            Register-Fail $Label "executable missing"
            break
        }

        Write-Host "[$Label] Exe : $ExePath"
        Write-Host "[$Label] Log : $LogFile"
        if ($ExtraArgs.Count -gt 0) { Write-Host "[$Label] Args: $($ExtraArgs -join ' ')" }

        $exitCode = Invoke-ExeSync -ExePath $ExePath -WorkDir $WorkDir -LogFile $LogFile -ExtraArgs $ExtraArgs
        $errorLines = Test-LogForErrors -LogFile $LogFile
        $structuredCases = Get-StructuredTestCaseResults -LogFile $LogFile

        if (@($structuredCases).Count -eq 0) {
            Register-Subtest -Group $Label -Name "StructuredTestcaseMarkers" -Status "FAILED" -Reason $missingMarkerReason
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
        Get-Content $LogFile -Tail 10 | ForEach-Object { Write-Host "  $_" }

        $structuredFailures = @($structuredCases | Where-Object { $_.Status -eq "FAILED" })
        if ($exitCode -eq 0 -and @($structuredCases).Count -gt 0 -and @($structuredFailures).Count -eq 0 -and @($errorLines).Count -eq 0) {
            Register-Pass $Label
        } else {
            $reason = if ($exitCode -ne 0) {
                "exit $exitCode"
            } elseif (@($structuredCases).Count -eq 0) {
                $missingMarkerReason
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

function Invoke-ExeTimed {
    param(
        [Parameter(Mandatory)][string]   $ExePath,
        [Parameter(Mandatory)][string]   $WorkDir,
        [Parameter(Mandatory)][string]   $LogFile,
        [string]  $ErrFile     = "$LogFile.stderr.tmp",
        [string[]]$ExtraArgs   = @(),
        [int]     $TimeoutSec  = 20
    )

    $p = @{
        FilePath               = $ExePath
        WorkingDirectory       = $WorkDir
        RedirectStandardOutput = $LogFile
        RedirectStandardError  = $ErrFile
        PassThru               = $true
        NoNewWindow            = $true
    }
    if ($ExtraArgs.Count -gt 0) { $p['ArgumentList'] = $ExtraArgs }

    $proc = Start-Process @p
    $survived = $proc.WaitForExit($TimeoutSec * 1000)

    $script:TimedExitCode = 0
    if (-not $survived) {
        $killOk = Stop-ProcessTree -ProcessId $proc.Id
        if (-not $killOk) {
            Write-Host "[ERROR] Failed to kill process tree for PID $($proc.Id)" -ForegroundColor Red
        }
        $proc.WaitForExit()
        if (-not (Test-ProcessExited -ProcessId $proc.Id)) {
            Write-Host "[ERROR] Process tree still running for PID $($proc.Id)" -ForegroundColor Red
            $script:TimedExitCode = -1
            return $false
        }
        return $true
    } else {
        $exitCode = $proc.ExitCode
        if ($null -eq $exitCode) { $exitCode = 0 }
        $script:TimedExitCode = $exitCode
        return $false
    }
}

function Test-LogForErrors {
    param([Parameter(Mandatory)][string]$LogFile)

    if (-not (Test-Path $LogFile)) { return @() }

    $p1 = "(?-i)\[(error|critical)\](?!.*\[Loader Message\]).*(crash|exception|fatal|access.violation|assert)"
    $p2 = "(?-i)(Validation Error\s*:|VUID-|Shader Compile Error|Assertion failed|Fatal : VkResult)"
    $combined = "$p1|$p2"

    $hits = Select-String -Path $LogFile -Pattern $combined -CaseSensitive
    return @($hits)
}

function Get-DescriptorHeapCapabilityState {
    param([Parameter(Mandatory)][string]$LogFile)

    if (-not (Test-Path $LogFile)) {
        return [PSCustomObject]@{ State = "Unknown"; Reason = "log missing" }
    }

    $enabled = Select-String -Path $LogFile -Pattern "Descriptor Heap initialized" -CaseSensitive:$false
    if ($enabled) {
        return [PSCustomObject]@{ State = "Enabled"; Reason = "runtime reported descriptor heap initialization" }
    }

    $skipped = Select-String -Path $LogFile -Pattern "descriptor heap.*(unsupported|disabled|skip|not supported)" -CaseSensitive:$false
    if ($skipped) {
        return [PSCustomObject]@{ State = "Skipped"; Reason = ($skipped | Select-Object -First 1).Line }
    }

    return [PSCustomObject]@{ State = "Unknown"; Reason = "runtime did not report descriptor heap capability state" }
}

function Get-StructuredTestCaseResults {
    param([Parameter(Mandatory)][string]$LogFile)

    if (-not (Test-Path $LogFile)) { return @() }

    $results = @()
    $pattern = '\[TESTCASE\]\[(PASS|FAIL|SKIP)\]\s+([A-Za-z0-9_.-]+)(?:\s*::\s*(.*))?'
    foreach ($line in Get-Content $LogFile) {
        if ($line -match $pattern) {
            $status = switch ($matches[1]) {
                "PASS" { "PASSED" }
                "FAIL" { "FAILED" }
                default { "SKIPPED" }
            }
            $results += [PSCustomObject]@{
                Name   = $matches[2]
                Status = $status
                Reason = $matches[3]
            }
        }
    }

    return @($results)
}

function Save-Minidumps([string]$BinDir) {
    foreach ($ext in "*.dmp","*.mdmp") {
        Get-ChildItem $BinDir -Filter $ext -ErrorAction SilentlyContinue |
            ForEach-Object {
                Write-Host "[Crash] Minidump saved: $($_.Name)" -ForegroundColor Red
                Write-Summary "[Crash] Minidump: $($_.Name)"
                Copy-Item $_.FullName $script:RunDir
            }
    }
}

function Register-Pass([string]$Label) {
    Write-ResultLine -Scope "Test" -Label $Label -Status "PASSED"
    $script:PassCount++
}

function Register-Fail([string]$Label, [string]$Reason = "") {
    Write-ResultLine -Scope "Test" -Label $Label -Status "FAILED" -Reason $Reason
    $script:FailCount++
}

function Register-Skip([string]$Label, [string]$Reason = "") {
    Write-ResultLine -Scope "Test" -Label $Label -Status "SKIPPED" -Reason $Reason
    $script:SkipCount++
}

function Register-Subtest(
    [Parameter(Mandatory)][string]$Group,
    [Parameter(Mandatory)][string]$Name,
    [Parameter(Mandatory)][ValidateSet("PASSED","FAILED","SKIPPED")][string]$Status,
    [string]$Reason = ""
) {
    Write-ResultLine -Scope "$Group/$Name" -Label "Result" -Status $Status -Reason $Reason
    switch ($Status) {
        "PASSED" { $script:SubPassCount++ }
        "FAILED" { $script:SubFailCount++ }
        "SKIPPED" { $script:SubSkipCount++ }
    }
}

function Finish-TestRun {
    Write-Summary "============================================================"
    Write-Summary " TESTS   : PASSED=$($script:PassCount)   FAILED=$($script:FailCount)   SKIPPED=$($script:SkipCount)"
    Write-Summary " SUBTEST : PASSED=$($script:SubPassCount)   FAILED=$($script:SubFailCount)   SKIPPED=$($script:SubSkipCount)"
    Write-Summary "============================================================"

    $color = if ($script:FailCount -gt 0) { "Red" } else { "Green" }
    Write-Host "============================================================" -ForegroundColor $color
    Write-Host " TESTS   : PASSED=$($script:PassCount)   FAILED=$($script:FailCount)   SKIPPED=$($script:SkipCount)" -ForegroundColor $color
    Write-Host " SUBTEST : PASSED=$($script:SubPassCount)   FAILED=$($script:SubFailCount)   SKIPPED=$($script:SubSkipCount)" -ForegroundColor $color
    Write-Host " Logs   :  $($script:RunDir)"
    Write-Host "============================================================"
    Write-Host ""

    exit $(if ($script:FailCount -gt 0) { 1 } else { 0 })
}