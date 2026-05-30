#Requires -Version 5.1
param(
    [ValidateSet("Debug","Release","RelWithDebInfo")]
    [string]$Config = "Debug",

    [int]$TimeoutSec = 20,

    [string[]]$ExtraArgs = @()
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. "$PSScriptRoot\common.ps1"

Initialize-TestRun -Config $Config -ScriptDir $PSScriptRoot

$BinDir = Join-Path $script:Root "target\bin\$Config"
$Exe    = Join-Path $BinDir "MoerEditor.exe"
$Label  = "Editor"

Write-Host "--- MoerEditor Test ($TimeoutSec s) ---" -ForegroundColor Yellow

do {
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
    Write-Host "[$Label] Will kill after $TimeoutSec s..."

    $RunStart = Get-Date
    $survived = Invoke-ExeTimed -ExePath $Exe -WorkDir $BinDir -LogFile $Log `
                                -ExtraArgs $ExtraArgs -TimeoutSec $TimeoutSec
    Merge-Stderr $Log "$Log.stderr.tmp"

    $failed = $false
    if ($survived) {
        Write-Host "[$Label] Killed after $TimeoutSec s."
    } else {
        Write-Host "[$Label] Process exited early (code: $($script:TimedExitCode))." -ForegroundColor DarkYellow
        Write-Summary "[$Label] Process exited early (code: $($script:TimedExitCode))."
        if ($script:TimedExitCode -ne 0) { $failed = $true }
    }

    $errorLines = Test-LogForErrors -LogFile $Log
    $descriptorHeapCapability = Get-DescriptorHeapCapabilityState -LogFile $Log
    $runtimeUnsupported = $descriptorHeapCapability.State -eq "Skipped"

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
Finish-TestRun