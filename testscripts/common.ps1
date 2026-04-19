#Requires -Version 5.1
<#
.SYNOPSIS
    Shared utilities for MoerEngine test scripts.
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

# ─── Shared state (populated by Initialize-TestRun) ──────────────────────────
$script:RunDir      = $null
$script:SummaryFile = $null
$script:PassCount   = 0
$script:FailCount   = 0
$script:SkipCount   = 0
$script:SubPassCount = 0
$script:SubFailCount = 0
$script:SubSkipCount = 0
$script:Config      = "Debug"
$script:Root        = $null

# ─────────────────────────────────────────────────────────────────────────────
function Initialize-TestRun {
<#
.SYNOPSIS
    Must be called at the start of every test script.
    Creates the per-run logs/<run_STAMP>/ directory.
#>
    param(
        [string]$Config    = "Debug",
        [string]$ScriptDir = $PSScriptRoot        # caller passes $PSScriptRoot
    )

    $script:Config = $Config
    $script:Root   = Split-Path $ScriptDir -Parent  # testscripts/ -> repo root

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

# ─────────────────────────────────────────────────────────────────────────────
function Write-Summary([string]$Line) {
    Add-Content -Path $script:SummaryFile -Value $Line -Encoding UTF8
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

# ─────────────────────────────────────────────────────────────────────────────
function Build-Target {
<#
.SYNOPSIS
    Build a CMake target under build/ before test execution.
#>
    param(
        [Parameter(Mandatory)][string]$Target
    )

    $BuildDir = Join-Path $script:Root "build"
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

# ─────────────────────────────────────────────────────────────────────────────
function Assert-Exe([string]$ExePath) {
<#
.SYNOPSIS  Returns $true if the exe exists; prints error + increments FailCount otherwise.
#>
    if (-not (Test-Path $ExePath)) {
        Write-Host "[ERROR] Not found: $ExePath" -ForegroundColor Red
        Write-Host "        Build first: cmake --build build --config $($script:Config)"
        Write-Summary "[ERROR] Not found: $ExePath"
        $script:FailCount++
        return $false
    }
    return $true
}

# ─────────────────────────────────────────────────────────────────────────────
function Merge-Stderr([string]$MainLog, [string]$ErrLog) {
<#
.SYNOPSIS  Appends non-empty stderr file to MainLog, then deletes ErrLog.
#>
    if (Test-Path $ErrLog) {
        if ((Get-Item $ErrLog).Length -gt 0) {
            Add-Content $MainLog "`n--- stderr ---"
            Get-Content $ErrLog | Add-Content $MainLog
        }
        Remove-Item $ErrLog -ErrorAction SilentlyContinue
    }
}

function Stop-ProcessTree {
<#
.SYNOPSIS
    Force-kills a process and its child process tree.
#>
    param([Parameter(Mandatory)][int]$ProcessId)

    $taskkill = Start-Process -FilePath "taskkill.exe" -ArgumentList @("/PID", "$ProcessId", "/T", "/F") -NoNewWindow -PassThru -Wait
    return $taskkill.ExitCode -eq 0 -or $taskkill.ExitCode -eq 128 -or $taskkill.ExitCode -eq 255
}

function Test-ProcessExited {
<#
.SYNOPSIS
    Returns $true when a process id no longer exists.
#>
    param([Parameter(Mandatory)][int]$ProcessId)

    return $null -eq (Get-Process -Id $ProcessId -ErrorAction SilentlyContinue)
}

# ─────────────────────────────────────────────────────────────────────────────
function Invoke-ExeSync {
<#
.SYNOPSIS
    Run an executable synchronously; redirect stdout+stderr to log files.
    Returns the process exit code.
#>
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

# ─────────────────────────────────────────────────────────────────────────────
function Invoke-ExeTimed {
<#
.SYNOPSIS
    Start an executable and kill it after $TimeoutSec seconds.
    Returns $true if the process was still running at timeout (normal),
            $false if it exited early (possible crash).
    Sets $script:TimedExitCode to the process exit code (0 if killed).
#>
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
        return $true   # killed at timeout = normal
    } else {
        $script:TimedExitCode = $proc.ExitCode
        return $false  # exited early
    }
}

# ─────────────────────────────────────────────────────────────────────────────
function Test-LogForErrors {
<#
.SYNOPSIS
    Scans a log file for two categories of error and returns all matched lines.

    Category 1 — Application crashes / fatal errors:
      Matches spdlog [error]-level lines containing crash/exception/fatal/etc.
      Excludes Vulkan Loader info messages (which only appear at [error] level
      due to the debug callback but don't indicate application errors).

    Category 2 — Vulkan validation layer hard errors:
      "Validation Error: [ VUID-... ]"   — hard validation failure
      "[N] [VUID-...]" detail lines are not treated as failure on their own.

    Returns an array of MatchInfo objects (empty array if no issues found).
    Callers should check  ($results.Count -gt 0)  to detect problems.
#>
    param([Parameter(Mandatory)][string]$LogFile)

    if (-not (Test-Path $LogFile)) { return @() }

    # Pattern 1: spdlog [error] lines with crash/exception/fatal keywords.
    # The negative look-ahead (?!.*Loader Message) avoids Vulkan Loader spam
    # that happens to be routed through the debug callback at [error] severity.
    $p1 = "(?-i)\[error\](?!.*\[Loader Message\]).*(crash|exception|fatal|access.violation|assert)"

    # Pattern 2: Vulkan validation layer hard errors only.
    $p2 = "(?-i)(Validation Error\s*:)"

    $combined = "$p1|$p2"

    $hits = Select-String -Path $LogFile -Pattern $combined -CaseSensitive
    return @($hits)
}

function Get-DescriptorHeapCapabilityState {
<#
.SYNOPSIS
    Parses a runtime log and returns one of: Enabled, Skipped, Unknown.
#>
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
<#
.SYNOPSIS
    Parses standardized per-case markers from a runtime log.

.DESCRIPTION
    Expected lines:
      [TESTCASE][PASS] CaseName
      [TESTCASE][FAIL] CaseName :: reason
      [TESTCASE][SKIP] CaseName :: reason
#>
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

# ─────────────────────────────────────────────────────────────────────────────
function Save-Minidumps([string]$BinDir) {
<#
.SYNOPSIS  Copy any *.dmp / *.mdmp from BinDir into the run log folder.
#>
    foreach ($ext in "*.dmp","*.mdmp") {
        Get-ChildItem $BinDir -Filter $ext -ErrorAction SilentlyContinue |
            ForEach-Object {
                Write-Host "[Crash] Minidump saved: $($_.Name)" -ForegroundColor Red
                Write-Summary "[Crash] Minidump: $($_.Name)"
                Copy-Item $_.FullName $script:RunDir
            }
    }
}

# ─────────────────────────────────────────────────────────────────────────────
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

# ─────────────────────────────────────────────────────────────────────────────
function Finish-TestRun {
<#
.SYNOPSIS  Print final banner, write summary footer, and exit with correct code.
#>
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
