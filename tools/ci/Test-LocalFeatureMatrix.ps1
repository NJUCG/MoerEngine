#Requires -Version 5.1
[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release", "RelWithDebInfo")]
    [string]$Config = "Release",

    [ValidateSet("All", "Core")]
    [string]$Scope = "All",

    [string]$BuildRoot,
    [string]$ResultRoot,
    [string]$LibTorchDir,
    [string]$TensorRtDir,
    [string]$NrdRoot,
    [string]$CCompiler = "clang",
    [string]$CxxCompiler = "clang++",
    [string]$Python = "python",
    [ValidateRange(1, 128)]
    [int]$Parallel = 16,
    [switch]$SkipRuntime,
    [switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$RepoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\.."))
$CombinationScript = Join-Path $PSScriptRoot "Invoke-FeatureValidation.ps1"
$RunId = Get-Date -Format "yyyyMMdd_HHmmss"

function Resolve-OutputPath {
    param(
        [string]$Value,
        [Parameter(Mandatory = $true)][string]$DefaultRelativePath
    )

    if ([string]::IsNullOrWhiteSpace($Value)) {
        return [System.IO.Path]::GetFullPath((Join-Path $RepoRoot $DefaultRelativePath))
    }
    if ([System.IO.Path]::IsPathRooted($Value)) {
        return [System.IO.Path]::GetFullPath($Value)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $RepoRoot $Value))
}

$BuildRoot = Resolve-OutputPath `
    -Value $BuildRoot `
    -DefaultRelativePath "build\local-feature-matrix"
$ResultRoot = Resolve-OutputPath `
    -Value $ResultRoot `
    -DefaultRelativePath "target\validation\local-feature-matrix\$RunId"

if (-not (Test-Path -LiteralPath $CombinationScript -PathType Leaf)) {
    throw "Combination validation script is missing: $CombinationScript"
}

$Combinations = @(
    [pscustomobject]@{ Id = "cuda-off-nrd-off"; Cuda = "OFF"; Nrd = "OFF" }
)
if ($Scope -eq "All") {
    $Combinations += @(
        [pscustomobject]@{ Id = "cuda-off-nrd-on"; Cuda = "OFF"; Nrd = "ON" },
        [pscustomobject]@{ Id = "cuda-on-nrd-off"; Cuda = "ON"; Nrd = "OFF" },
        [pscustomobject]@{ Id = "cuda-on-nrd-on"; Cuda = "ON"; Nrd = "ON" }
    )
}

function New-ValidationArguments {
    param(
        [Parameter(Mandatory = $true)][string]$WithCuda,
        [Parameter(Mandatory = $true)][string]$WithNrd,
        [Parameter(Mandatory = $true)][string]$BuildDir,
        [Parameter(Mandatory = $true)][string]$ValidationDir
    )

    $Arguments = @{
        WithCuda     = $WithCuda
        WithNrd      = $WithNrd
        Config       = $Config
        BuildDir     = $BuildDir
        ValidationDir = $ValidationDir
        CCompiler    = $CCompiler
        CxxCompiler  = $CxxCompiler
        Python       = $Python
        Parallel     = $Parallel
    }
    if (-not [string]::IsNullOrWhiteSpace($LibTorchDir)) {
        $Arguments.LibTorchDir = $LibTorchDir
    }
    if (-not [string]::IsNullOrWhiteSpace($TensorRtDir)) {
        $Arguments.TensorRtDir = $TensorRtDir
    }
    if (-not [string]::IsNullOrWhiteSpace($NrdRoot)) {
        $Arguments.NrdRoot = $NrdRoot
    }
    if ($SkipRuntime) {
        $Arguments.SkipRuntime = $true
    }
    return $Arguments
}

Write-Host "MoerEngine local optional-feature matrix" -ForegroundColor Cyan
Write-Host "  Scope:      $Scope"
Write-Host "  Config:     $Config"
Write-Host "  Build root: $BuildRoot"
Write-Host "  Results:    $ResultRoot"

if (-not $DryRun) {
    # Check the strictest requested combination once so a missing optional SDK
    # fails immediately instead of after one or more lengthy builds.
    $PreflightCuda = if ($Scope -eq "All") { "ON" } else { "OFF" }
    $PreflightNrd = if ($Scope -eq "All") { "ON" } else { "OFF" }
    $PreflightArguments = New-ValidationArguments `
        -WithCuda $PreflightCuda `
        -WithNrd $PreflightNrd `
        -BuildDir (Join-Path $BuildRoot "_preflight") `
        -ValidationDir (Join-Path $ResultRoot "_preflight")
    $PreflightArguments.PreflightOnly = $true

    Write-Host "`nChecking local toolchain and optional dependencies..." -ForegroundColor Cyan
    & $CombinationScript @PreflightArguments
    New-Item -ItemType Directory -Path $ResultRoot -Force | Out-Null
}

$Results = New-Object System.Collections.Generic.List[object]
foreach ($Combination in $Combinations) {
    $BuildDir = Join-Path $BuildRoot $Combination.Id
    $ValidationDir = Join-Path $ResultRoot $Combination.Id
    $Arguments = New-ValidationArguments `
        -WithCuda $Combination.Cuda `
        -WithNrd $Combination.Nrd `
        -BuildDir $BuildDir `
        -ValidationDir $ValidationDir

    if ($DryRun) {
        Write-Host "[DRY-RUN] $($Combination.Id)" -ForegroundColor DarkCyan
        $Results.Add([pscustomobject]@{
            Combination = $Combination.Id
            Result      = "DRY-RUN"
            Seconds     = "0.0"
            Details     = "Not executed"
        })
        continue
    }

    Write-Host "`n============================================================" -ForegroundColor DarkGray
    Write-Host "Testing $($Combination.Id)" -ForegroundColor Cyan
    Write-Host "============================================================" -ForegroundColor DarkGray

    $Stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    $Result = "PASS"
    $Details = "OK"
    try {
        & $CombinationScript @Arguments
    } catch {
        $Result = "FAIL"
        $Details = $_.Exception.Message
        Write-Host "FAILED: $Details" -ForegroundColor Red
    } finally {
        $Stopwatch.Stop()
    }

    $Results.Add([pscustomobject]@{
        Combination = $Combination.Id
        Result      = $Result
        Seconds     = $Stopwatch.Elapsed.TotalSeconds.ToString("F1")
        Details     = $Details
    })
}

Write-Host "`nLocal feature matrix summary" -ForegroundColor Cyan
$Results | Format-Table Combination, Result, Seconds, Details -AutoSize

if (-not $DryRun) {
    $SummaryPath = Join-Path $ResultRoot "summary.md"
    $SummaryLines = New-Object System.Collections.Generic.List[string]
    $SummaryLines.Add("# MoerEngine Local Feature Matrix")
    $SummaryLines.Add("")
    $SummaryLines.Add("- Run: ``$RunId``")
    $SummaryLines.Add("- Configuration: ``$Config``")
    $SummaryLines.Add("- Runtime validation: ``$(-not $SkipRuntime)``")
    $SummaryLines.Add("")
    $SummaryLines.Add("| Combination | Result | Seconds | Details |")
    $SummaryLines.Add("|---|---|---:|---|")
    foreach ($Item in $Results) {
        $SafeDetails = ([string]$Item.Details) -replace "[\r\n]+", " " -replace "\|", "\\|"
        $SummaryLines.Add("| $($Item.Combination) | $($Item.Result) | $($Item.Seconds) | $SafeDetails |")
    }
    $Utf8WithoutBom = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText(
        $SummaryPath,
        ($SummaryLines -join [Environment]::NewLine) + [Environment]::NewLine,
        $Utf8WithoutBom
    )
    Write-Host "Summary: $SummaryPath"
}

$Failed = @($Results | Where-Object { $_.Result -eq "FAIL" })
if ($Failed.Count -gt 0) {
    throw "Local feature matrix failed: $($Failed.Combination -join ', ')"
}

if (-not $DryRun) {
    Write-Host "All requested feature combinations passed." -ForegroundColor Green
}
