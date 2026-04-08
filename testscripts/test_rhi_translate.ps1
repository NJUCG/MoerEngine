#Requires -Version 5.1
<#
.SYNOPSIS
    Run TestRHITranslate.exe (RHI multi-queue translate test).

.PARAMETER Config
    Build configuration: Debug (default), Release, RelWithDebInfo

.PARAMETER ExtraArgs
    Extra arguments forwarded to the executable.

.EXAMPLE
    .\test_rhi_translate.ps1
    .\test_rhi_translate.ps1 -Config Release
    .\test_rhi_translate.ps1 -ExtraArgs @("-DENABLE_VALIDATION=1")
#>
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
$Exe    = Join-Path $BinDir "TestRHITranslate.exe"
$Label  = "RHI"

Write-Host "--- RHI Translate Multi-Queue Test ---" -ForegroundColor Yellow

do {
    if (-not (Build-Target -Target "TestRHITranslate")) { break }
    if (-not (Assert-Exe $Exe)) { break }

    $Log = Join-Path $script:RunDir "rhi_translate.log"

    Write-Host "[$Label] Exe : $Exe"
    Write-Host "[$Label] Log : $Log"
    if ($ExtraArgs.Count -gt 0) { Write-Host "[$Label] Args: $($ExtraArgs -join ' ')" }

    $exitCode = Invoke-ExeSync -ExePath $Exe -WorkDir $BinDir -LogFile $Log -ExtraArgs $ExtraArgs

    # Print tail for quick feedback
    Write-Host "[$Label] Output tail:"
    Get-Content $Log -Tail 10 | ForEach-Object { Write-Host "  $_" }

    if ($exitCode -eq 0) {
        Register-Pass $Label
    } else {
        # Surface failure lines in console and summary
        $fails = Select-String -Path $Log -Pattern "mismatch|failed|\[error\]" -CaseSensitive:$false
        $fails | ForEach-Object { Write-Host "  $($_.Line)" }
        Register-Fail $Label "exit $exitCode"
    }
} while ($false)

Write-Host ""
Finish-TestRun
