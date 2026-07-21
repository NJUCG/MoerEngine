#Requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("ON", "OFF")]
    [string]$WithCuda,

    [Parameter(Mandatory = $true)]
    [ValidateSet("ON", "OFF")]
    [string]$WithNrd,

    [ValidateSet("Debug", "Release", "RelWithDebInfo")]
    [string]$Config = "Release",

    [string]$BuildDir,
    [string]$ValidationDir,
    [string]$LibTorchDir,
    [string]$TensorRtDir,
    [string]$NrdRoot,
    [string]$CCompiler = "clang",
    [string]$CxxCompiler = "clang++",
    [string]$Python = "python",
    [ValidateRange(1, 128)]
    [int]$Parallel = 16,
    [switch]$SkipRuntime,
    [switch]$PreflightOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$RepoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\.."))
$MatrixId = "cuda-$($WithCuda.ToLowerInvariant())-nrd-$($WithNrd.ToLowerInvariant())"

if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $BuildDir = Join-Path $RepoRoot "build\local-feature-matrix\$MatrixId"
}
$BuildDir = [System.IO.Path]::GetFullPath($BuildDir)
$BinaryRoot = Join-Path $BuildDir "output"

if ([string]::IsNullOrWhiteSpace($ValidationDir)) {
    $Timestamp = Get-Date -Format "yyyyMMdd_HHmmss"
    $ValidationDir = Join-Path $RepoRoot "target\validation\feature-matrix\$MatrixId-$Timestamp"
}
$ValidationDir = [System.IO.Path]::GetFullPath($ValidationDir)

function Assert-Command {
    param([Parameter(Mandatory = $true)][string]$Name)

    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        throw "Required command is unavailable: $Name"
    }
}

function Import-VisualStudioBuildEnvironment {
    $VsWhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path -LiteralPath $VsWhere -PathType Leaf)) {
        throw "Windows resource compiler is unavailable and vswhere.exe was not found."
    }

    $InstallRoots = @(& $VsWhere `
        -latest `
        -products "*" `
        -property installationPath)
    $VsWhereExitCode = $LASTEXITCODE
    $InstallRoot = $InstallRoots | Select-Object -First 1
    if ($VsWhereExitCode -ne 0 -or [string]::IsNullOrWhiteSpace($InstallRoot)) {
        throw "A Visual Studio installation was not found."
    }

    $VsDevCmd = Join-Path $InstallRoot "Common7\Tools\VsDevCmd.bat"
    if (-not (Test-Path -LiteralPath $VsDevCmd -PathType Leaf)) {
        throw "Visual Studio developer environment script was not found: $VsDevCmd"
    }

    $Command = "`"$VsDevCmd`" -no_logo -arch=x64 -host_arch=x64 && set"
    $EnvironmentLines = & $env:ComSpec /d /s /c $Command
    if ($LASTEXITCODE -ne 0) {
        throw "VsDevCmd.bat failed with exit code $LASTEXITCODE"
    }
    foreach ($Line in $EnvironmentLines) {
        if ($Line -match "^([^=]+)=(.*)$") {
            [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], "Process")
        }
    }
}

function Resolve-CompilerCommand {
    param(
        [Parameter(Mandatory = $true)][string]$ConfiguredName,
        [Parameter(Mandatory = $true)][string]$ExecutableName
    )

    if (Get-Command $ConfiguredName -ErrorAction SilentlyContinue) {
        return $ConfiguredName
    }

    $VsWhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $VsWhere -PathType Leaf) {
        $InstallRoots = @(& $VsWhere -all -products "*" -property installationPath)
        foreach ($InstallRoot in $InstallRoots) {
            foreach ($RelativePath in @(
                "VC\Tools\Llvm\x64\bin\$ExecutableName",
                "VC\Tools\Llvm\bin\$ExecutableName"
            )) {
                $Candidate = Join-Path $InstallRoot $RelativePath
                if (Test-Path -LiteralPath $Candidate -PathType Leaf) {
                    return $Candidate
                }
            }
        }
    }

    throw "Required compiler is unavailable: $ConfiguredName"
}

function Resolve-ConfiguredDirectory {
    param(
        [string]$ExplicitValue,
        [Parameter(Mandatory = $true)][string[]]$EnvironmentNames,
        [Parameter(Mandatory = $true)][string]$Label
    )

    $Value = $ExplicitValue
    if ([string]::IsNullOrWhiteSpace($Value)) {
        foreach ($Name in $EnvironmentNames) {
            $Candidate = [Environment]::GetEnvironmentVariable($Name)
            if (-not [string]::IsNullOrWhiteSpace($Candidate)) {
                $Value = $Candidate
                break
            }
        }
    }
    if ([string]::IsNullOrWhiteSpace($Value)) {
        throw "$Label is required. Pass it explicitly or configure one of: $($EnvironmentNames -join ', ')."
    }

    $Resolved = [System.IO.Path]::GetFullPath($Value)
    if (-not (Test-Path -LiteralPath $Resolved -PathType Container)) {
        throw "$Label directory does not exist: $Resolved"
    }
    return $Resolved
}

function Invoke-External {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )

    Write-Host "> $FilePath $($Arguments -join ' ')" -ForegroundColor Cyan
    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$FilePath failed with exit code $LASTEXITCODE"
    }
}

if ($env:OS -ne "Windows_NT") {
    throw "MoerEngine feature validation requires Windows."
}

# clang's GNU-like Windows driver still needs rc.exe, link.exe, SDK includes and
# SDK libraries. GitHub runner processes do not normally inherit a VS Developer
# Prompt, so import it when the resource compiler is missing.
if (-not (Get-Command "rc.exe" -ErrorAction SilentlyContinue)) {
    Import-VisualStudioBuildEnvironment
}

$CCompiler = Resolve-CompilerCommand -ConfiguredName $CCompiler -ExecutableName "clang.exe"
$CxxCompiler = Resolve-CompilerCommand -ConfiguredName $CxxCompiler -ExecutableName "clang++.exe"

Assert-Command "cmake"
Assert-Command "ninja"
Assert-Command "rc.exe"
Assert-Command $CCompiler
Assert-Command $CxxCompiler
Assert-Command $Python

$CmakeArgs = @(
    "-S", $RepoRoot,
    "-B", $BuildDir,
    "-G", "Ninja",
    "-DCMAKE_BUILD_TYPE=$Config",
    "-DCMAKE_C_COMPILER=$CCompiler",
    "-DCMAKE_CXX_COMPILER=$CxxCompiler",
    "-DBINARY_ROOT_DIR=$BinaryRoot",
    "-Dmoer_build_test=OFF",
    "-DMOER_IGNORE_ENABLE_FEATURES=ON",
    "-DWITH_CUDA=$WithCuda",
    "-DWITH_NRD=$WithNrd",
    "-DWITH_RENDERDOC=OFF",
    "-DWITH_PROFILE=OFF"
)

$RuntimePathEntries = New-Object System.Collections.Generic.List[string]
if ($WithCuda -eq "ON") {
    Assert-Command "nvcc"
    $LibTorchDir = Resolve-ConfiguredDirectory `
        -ExplicitValue $LibTorchDir `
        -EnvironmentNames @("MOER_FEATURE_LIBTORCH_DIR", "MOER_CI_LIBTORCH_DIR", "LIBTORCH_DIR") `
        -Label "LibTorch"
    $TensorRtDir = Resolve-ConfiguredDirectory `
        -ExplicitValue $TensorRtDir `
        -EnvironmentNames @("MOER_FEATURE_TENSORRT_DIR", "MOER_CI_TENSORRT_DIR", "TENSORRT_DIR") `
        -Label "TensorRT"

    $TorchConfig = Join-Path $LibTorchDir "share\cmake\Torch\TorchConfig.cmake"
    $TensorRtHeader = Join-Path $TensorRtDir "include\NvInfer.h"
    if (-not (Test-Path -LiteralPath $TorchConfig -PathType Leaf)) {
        throw "LibTorch CMake package is missing: $TorchConfig"
    }
    if (-not (Test-Path -LiteralPath $TensorRtHeader -PathType Leaf)) {
        throw "TensorRT header is missing: $TensorRtHeader"
    }

    $CmakeArgs += "-DLIBTORCH_DIR=$LibTorchDir"
    $CmakeArgs += "-DTENSORRT_DIR=$TensorRtDir"
    foreach ($RuntimeLibDir in @(
        (Join-Path $LibTorchDir "lib"),
        (Join-Path $TensorRtDir "lib")
    )) {
        if (-not (Test-Path -LiteralPath $RuntimeLibDir -PathType Container)) {
            throw "Runtime library directory does not exist: $RuntimeLibDir"
        }
        $RuntimePathEntries.Add($RuntimeLibDir)
    }
}

if ($WithNrd -eq "ON") {
    $NrdRoot = Resolve-ConfiguredDirectory `
        -ExplicitValue $NrdRoot `
        -EnvironmentNames @("MOER_FEATURE_NRD_ROOT", "MOER_CI_NRD_ROOT", "NRD_ROOT") `
        -Label "NRD"
    $NrdCmake = Join-Path $NrdRoot "CMakeLists.txt"
    if (-not (Test-Path -LiteralPath $NrdCmake -PathType Leaf)) {
        throw "NRD source tree is missing CMakeLists.txt: $NrdCmake"
    }
    $CmakeArgs += "-DNRD_ROOT=$NrdRoot"
}

if (-not $SkipRuntime) {
    Assert-Command "nvidia-smi"
}

if ($PreflightOnly) {
    Write-Host "Feature validation prerequisites are ready: $MatrixId" -ForegroundColor Green
    return
}

$RootConfig = Join-Path $RepoRoot "MoerEngine.toml"
if (-not (Test-Path -LiteralPath $RootConfig -PathType Leaf)) {
    Copy-Item -LiteralPath (Join-Path $RepoRoot "template.MoerEngine.toml") -Destination $RootConfig
}

Push-Location $RepoRoot
try {
    Invoke-External -FilePath "cmake" -Arguments $CmakeArgs

    $CachePath = Join-Path $BuildDir "CMakeCache.txt"
    $CacheText = Get-Content -LiteralPath $CachePath -Raw
    foreach ($Expected in @("WITH_CUDA:BOOL=$WithCuda", "WITH_NRD:BOOL=$WithNrd")) {
        if ($CacheText -notmatch "(?m)^$([regex]::Escape($Expected))\r?$") {
            throw "CMake cache does not contain the requested feature value: $Expected"
        }
    }

    Invoke-External -FilePath "cmake" -Arguments @(
        "--build", $BuildDir,
        "--config", $Config,
        "--target", "MoerEditor",
        "--parallel", [string]$Parallel
    )

    if ($SkipRuntime) {
        Write-Host "Feature build completed; runtime validation was skipped." -ForegroundColor Yellow
        return
    }

    Invoke-External -FilePath "nvidia-smi" -Arguments @("--query-gpu=name,driver_version", "--format=csv,noheader")

    if ($RuntimePathEntries.Count -gt 0) {
        $env:PATH = (($RuntimePathEntries.ToArray() + @($env:PATH)) -join [System.IO.Path]::PathSeparator)
    }

    $EditorExe = Join-Path $BinaryRoot "bin\$Config\MoerEditor.exe"
    if (-not (Test-Path -LiteralPath $EditorExe -PathType Leaf)) {
        throw "MoerEditor executable was not produced: $EditorExe"
    }

    Invoke-External -FilePath $Python -Arguments @(
        (Join-Path $RepoRoot "tools\threading\run_matrix.py"),
        "--set", "feature",
        "--exe", $EditorExe,
        "--workdir", (Split-Path -Parent $EditorExe),
        "--base-config", (Join-Path $RepoRoot "template.MoerEngine.toml"),
        "--outdir", $ValidationDir,
        "--skip-window-stress",
        "--startup-timeout", "180",
        "--ready-timeout", "300",
        "--close-timeout", "90"
    )
} finally {
    Pop-Location
}

Write-Host "Feature validation passed: $MatrixId" -ForegroundColor Green
Write-Host "Evidence: $ValidationDir"
