param(
    [string]$ProjectRoot = (Resolve-Path "$PSScriptRoot/../..").Path,
    [string]$OutputDirectory = '',
    [int]$Cpu = 2,
    [ValidateRange(1, 1000000)][int]$Samples = 600,
    [ValidateRange(0, 1000000)][int]$Warmup = 200
)
$ErrorActionPreference = 'Stop'
if (Get-Process MoerEditor,UnrealEditor,UnrealEditor-Cmd -ErrorAction SilentlyContinue) {
    throw 'Close other editors before collecting benchmark data.'
}
if (-not $OutputDirectory) { $OutputDirectory = "$ProjectRoot/build/rdg-profile-$(Get-Date -Format yyyyMMdd-HHmmss)" }
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Choose a new output directory to preserve previous evidence.' }
$baseline = "$ProjectRoot/build/rdg-release-target/bin/Release/TestRenderGraphCompilerBenchmark.exe"
$diagnostic = "$ProjectRoot/build/rdg-profile-target/bin/Release/TestRenderGraphCompilerBenchmark.exe"
foreach ($executable in @($baseline, $diagnostic)) {
    if (-not (Test-Path -LiteralPath $executable)) { throw "Missing benchmark: $executable" }
}
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$env:_NT_SYMBOL_PATH = (Split-Path $diagnostic)
$metadata = @{
    commit = (git -C $ProjectRoot rev-parse HEAD)
    changes = @(git -C $ProjectRoot status --short)
    cpu = $Cpu
    warmup = $Warmup
    samples = $Samples
    binaries = @(foreach ($executable in @($baseline, $diagnostic)) {
        foreach ($name in @('TestRenderGraphCompilerBenchmark.exe','moer_render.dll','moer_render.pdb','moer_core.dll','moer_core.pdb')) {
            Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path (Split-Path $executable) $name) | Select-Object Path,Hash
        }
    })
}
$metadata | ConvertTo-Json -Depth 5 | Set-Content -Encoding utf8 "$OutputDirectory/metadata.json"
function Invoke-Benchmark([string]$Executable, [string]$Name, [string[]]$Arguments) {
    & $Executable @Arguments 2> "$OutputDirectory/$Name.stderr.log" | Set-Content -Encoding utf8 "$OutputDirectory/$Name.json"
    if ($LASTEXITCODE -ne 0) { throw "Benchmark failed: $Name; see its stderr log." }
}
Invoke-Benchmark $diagnostic 'selftest' @('--diagnose','selftest')
foreach ($size in @(64,128)) {
    [string[]]$common = @('--cpu',"$Cpu",'--passes',"$size",'--resources',"$size",'--accesses','16','--warmup',"$Warmup",'--samples',"$Samples")
    foreach ($run in 1..3) {
        # Reverse the ordinary / instrumented order in the middle round.
        foreach ($variant in $(if ($run -eq 2) { @('inactive','baseline') } else { @('baseline','inactive') })) {
            $executable = if ($variant -eq 'baseline') { $baseline } else { $diagnostic }
            Invoke-Benchmark $executable "$variant-$size-$run" ($common + @('--lifecycle'))
        }
        Invoke-Benchmark $diagnostic "timing-$size-$run" ($common + @('--diagnose','timing'))
    }
    Write-Output "Timing and calibration complete: $size passes"
}
foreach ($shape in @('whole','subranges')) {
    [string[]]$shapeArgs = if ($shape -eq 'subranges') { @('--subranges') } else { @() }
    foreach ($size in @(16,64,128)) {
        $accesses = if ($size -eq 16) { 4 } else { 16 }
        foreach ($mode in @('operations','allocations')) {
            [string[]]$arguments = @('--diagnose',$mode,'--cpu',"$Cpu",'--passes',"$size",'--resources',"$size",'--accesses',"$accesses",'--warmup','3','--samples','3') + $shapeArgs
            Invoke-Benchmark $diagnostic "$mode-$shape-$size" $arguments
        }
    }
}
Write-Output "Results: $OutputDirectory"
