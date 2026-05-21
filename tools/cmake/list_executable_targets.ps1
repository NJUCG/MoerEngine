param(
    [string]$SourceDir = ".",
    [string]$BuildDir = "./build",
    [string]$Generator = "Ninja",
    [string]$CCompiler = "clang",
    [string]$CxxCompiler = "clang++"
)

$buildCache = Join-Path $BuildDir "CMakeCache.txt"
if (!(Test-Path $buildCache)) {
    cmake -B $BuildDir -G $Generator "-DCMAKE_C_COMPILER=$CCompiler" "-DCMAKE_CXX_COMPILER=$CxxCompiler" | Out-Null
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}

$queryDir = Join-Path $BuildDir ".cmake/api/v1/query"
New-Item -ItemType Directory -Force -Path $queryDir | Out-Null
New-Item -ItemType File -Force -Path (Join-Path $queryDir "codemodel-v2") | Out-Null

cmake -S $SourceDir -B $BuildDir | Out-Null
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

$replyDir = Join-Path $BuildDir ".cmake/api/v1/reply"
$index = Get-ChildItem $replyDir "index-*.json" | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($null -eq $index) {
    throw "CMake File API reply not found. Please run just generate first."
}

$indexJson = Get-Content $index.FullName -Raw | ConvertFrom-Json
$codemodelFile = Join-Path $replyDir $indexJson.reply.'codemodel-v2'.jsonFile
$codemodel = Get-Content $codemodelFile -Raw | ConvertFrom-Json

$targetNames = foreach ($configuration in $codemodel.configurations) {
    foreach ($targetRef in $configuration.targets) {
        $targetFile = Join-Path $replyDir $targetRef.jsonFile
        $target = Get-Content $targetFile -Raw | ConvertFrom-Json
        if ($target.type -eq "EXECUTABLE") {
            $target.name
        }
    }
}

$targetNames | Sort-Object -Unique