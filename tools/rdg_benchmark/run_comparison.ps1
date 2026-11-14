param(
    [string]$ProjectRoot = (Resolve-Path "$PSScriptRoot/../..").Path,
    [string]$UnrealRoot = 'D:/UE_5.7',
    [int]$Samples = 1000,
    [int]$Warmup = 100,
    [int]$Cpu = 2,
    [switch]$SkipUnreal
)
$ErrorActionPreference = 'Stop'
$results = "$ProjectRoot/build/rdg-ue-comparison"
$ueProject = "$ProjectRoot/build/ue-rdg-benchmark/RdgCpuBenchmark.uproject"
$moer = "$ProjectRoot/build/rdg-release-target/bin/Release/TestRenderGraphCompilerBenchmark.exe"
if (Get-Process MoerEditor,UnrealEditor,UnrealEditor-Cmd -ErrorAction SilentlyContinue) {
    throw 'Close other editors before measuring.'
}
New-Item -ItemType Directory -Path $results -Force | Out-Null
if (-not $SkipUnreal) {
    & "$UnrealRoot/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" $ueProject -run=RdgBenchmark -nullrhi -AllowCommandletRendering -unattended -nosplash -nosound -trace=none -Matrix "-Warmup=$Warmup" "-Samples=$Samples" "-Cpu=$Cpu" "-Output=$results/ue-matrix.json" "-abslog=$results/ue-matrix.log" *> "$results/ue-matrix-console.log"
    if ($LASTEXITCODE -ne 0) { throw 'UE benchmark failed; inspect ue-matrix-console.log.' }
    Write-Output 'UE matrix complete.'
}
for ($run = 1; $run -le 3; ++$run) {
    foreach ($case in @(@{P=16;A=4},@{P=64;A=16},@{P=128;A=16})) {
        foreach ($phase in @('lifecycle','compile','lower','record')) {
            [string[]]$phaseArgs = if ($phase -eq 'compile') { @() } else { @("--$phase") }
            & $moer @phaseArgs --passes $case.P --resources $case.P --accesses $case.A --warmup $Warmup --samples $Samples --cpu $Cpu | Set-Content -Encoding utf8 "$results/moer-$phase-$($case.P)-$run.json"
            if ($LASTEXITCODE -ne 0) { throw "Moer benchmark failed: $phase/$($case.P)/$run" }
        }
        Write-Output "Moer $($case.P) passes run $run complete."
    }
}
