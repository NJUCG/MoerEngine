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

Write-Host "--- [1/2] TaskGraph Regression ---" -ForegroundColor Yellow
Invoke-StructuredBinaryTest `
    -Label "TaskGraph" `
    -Target "TestTaskGraph" `
    -ExePath (Join-Path $BinDir "TestTaskGraph.exe") `
    -WorkDir $BinDir `
    -LogFile (Join-Path $script:RunDir "taskgraph.log") `
    -ExtraArgs $ExtraArgs

Write-Host "--- [2/2] TaskPipe Regression ---" -ForegroundColor Yellow
Invoke-StructuredBinaryTest `
    -Label "TaskPipe" `
    -Target "TestTaskPipe" `
    -ExePath (Join-Path $BinDir "TestTaskPipe.exe") `
    -WorkDir $BinDir `
    -LogFile (Join-Path $script:RunDir "taskpipe.log") `
    -ExtraArgs $ExtraArgs

Finish-TestRun