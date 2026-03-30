@echo off
setlocal enabledelayedexpansion

:: ============================================================
:: MoerEngine Quick Self-Check Test Script
:: Usage:
::   test_quick.bat [OPTIONS]
::
:: Options:
::   -config <Debug|Release|RelWithDebInfo>  Build config (default: Debug)
::   -editor                                 Run MoerEditor.exe test (20s)
::   -rhi                                    Run RHI translate multi-queue test
::   -all                                    Run all tests (default if no test flag)
::   -D<MACRO>[=<VALUE>]                     Pass extra args/defines (can repeat)
::   -help                                   Show this help
::
:: Examples:
::   test_quick.bat
::   test_quick.bat -all
::   test_quick.bat -editor -config Release
::   test_quick.bat -rhi -DENABLE_VALIDATION=1
::   test_quick.bat -all -config Debug -DDEBUG_QUEUES=1
:: ============================================================

:: --- Defaults ---
set CONFIG=Debug
set RUN_EDITOR=0
set RUN_RHI=0
set EXTRA_ARGS=
set SHOW_HELP=0

:: --- Parse arguments ---
:parse_args
if "%~1"=="" goto done_args
if /i "%~1"=="-help"   ( set SHOW_HELP=1 & shift & goto parse_args )
if /i "%~1"=="-config" ( set CONFIG=%~2  & shift & shift & goto parse_args )
if /i "%~1"=="-editor" ( set RUN_EDITOR=1 & shift & goto parse_args )
if /i "%~1"=="-rhi"    ( set RUN_RHI=1    & shift & goto parse_args )
if /i "%~1"=="-all"    ( set RUN_EDITOR=1 & set RUN_RHI=1 & shift & goto parse_args )
:: Collect any -D... or other extra args
set EXTRA_ARGS=!EXTRA_ARGS! %~1
shift & goto parse_args

:done_args

if %SHOW_HELP%==1 (
    type "%~f0" | findstr /b "::"
    exit /b 0
)

:: Default: run everything
if %RUN_EDITOR%==0 if %RUN_RHI%==0 (
    set RUN_EDITOR=1
    set RUN_RHI=1
)

:: --- Paths ---
set ROOT=%~dp0
set ROOT=%ROOT:~0,-1%
set BIN_DIR=%ROOT%\target\bin\%CONFIG%
set EDITOR_EXE=%BIN_DIR%\MoerEditor.exe
set RHI_EXE=%BIN_DIR%\TestRHITranslate.exe

:: --- Logs folder ---
set LOGS_DIR=%ROOT%\logs
if not exist "%LOGS_DIR%" mkdir "%LOGS_DIR%"

:: Timestamp: YYYYMMDD_HHMMSS
for /f "tokens=2 delims==" %%I in ('wmic os get localdatetime /value 2^>nul') do set DT=%%I
set STAMP=%DT:~0,8%_%DT:~8,6%
set RUN_DIR=%LOGS_DIR%\run_%STAMP%
mkdir "%RUN_DIR%"

set SUMMARY=%RUN_DIR%\summary.txt
set PASS_COUNT=0
set FAIL_COUNT=0

echo ============================================================ > "%SUMMARY%"
echo  MoerEngine Quick Test  [%STAMP%]                           >> "%SUMMARY%"
echo  Config  : %CONFIG%                                          >> "%SUMMARY%"
echo  Args    : %EXTRA_ARGS%                                      >> "%SUMMARY%"
echo ============================================================ >> "%SUMMARY%"

echo.
echo [MoerEngine Test]  %DATE% %TIME%
echo Config  : %CONFIG%
echo Logs    : %RUN_DIR%
echo.

:: ============================================================
:: Helper subroutine: verify exe exists
:: ============================================================
goto :skip_fn
:check_exe
    if not exist "%~1" (
        echo [ERROR] Not found: %~1
        echo [ERROR] Not found: %~1 >> "%SUMMARY%"
        echo         Build first: cmake --build build --config %CONFIG%
        exit /b 1
    )
    exit /b 0
:skip_fn

:: ============================================================
:: 1. RHI Translate Multi-Queue Test
:: ============================================================
if %RUN_RHI%==1 (
    echo --- [1/2] RHI Translate Multi-Queue Test ---
    call :check_exe "%RHI_EXE%"
    if errorlevel 1 ( set /a FAIL_COUNT+=1 & goto rhi_done )

    set RHI_LOG=%RUN_DIR%\rhi_translate.log
    set RHI_ERR=%RUN_DIR%\rhi_translate_stderr.log

    echo [RHI] Exe : %RHI_EXE%
    echo [RHI] Log : !RHI_LOG!
    if defined EXTRA_ARGS echo [RHI] Args: %EXTRA_ARGS%

    :: Run synchronously from the bin dir so toml/assets resolve correctly
    pushd "%BIN_DIR%"
    "%RHI_EXE%"%EXTRA_ARGS% > "!RHI_LOG!" 2> "!RHI_ERR!"
    set RHI_EXIT=!errorlevel!
    popd

    :: Merge non-empty stderr into main log
    for %%F in ("!RHI_ERR!") do if %%~zF GTR 0 (
        echo.                     >> "!RHI_LOG!"
        echo --- stderr ---       >> "!RHI_LOG!"
        type "!RHI_ERR!"         >> "!RHI_LOG!"
    )

    :: Print last few lines of log for quick feedback
    echo [RHI] Output tail:
    powershell -NoProfile -Command "Get-Content '!RHI_LOG!' -Tail 10 | Write-Host"

    if !RHI_EXIT! EQU 0 (
        echo [RHI] PASSED  ^(exit 0^)
        echo [RHI] PASSED  ^(exit 0^) >> "%SUMMARY%"
        set /a PASS_COUNT+=1
    ) else (
        echo [RHI] FAILED  ^(exit !RHI_EXIT!^)
        echo [RHI] FAILED  ^(exit !RHI_EXIT!^) >> "%SUMMARY%"
        echo [RHI] --- failure lines ---
        findstr /i "mismatch failed error" "!RHI_LOG!" 2>nul
        findstr /i "mismatch failed error" "!RHI_LOG!" >> "%SUMMARY%" 2>nul
        set /a FAIL_COUNT+=1
    )
    :rhi_done
    echo.
)

:: ============================================================
:: 2. MoerEditor Test (launch, wait 20 s, kill, inspect log)
:: ============================================================
if %RUN_EDITOR%==1 (
    echo --- [2/2] MoerEditor Test ^(20 s^) ---
    call :check_exe "%EDITOR_EXE%"
    if errorlevel 1 ( set /a FAIL_COUNT+=1 & goto editor_done )

    set EDITOR_LOG=%RUN_DIR%\moereditor.log
    set EDITOR_CRASH=%RUN_DIR%\moereditor_crash.txt

    echo [Editor] Exe : %EDITOR_EXE%
    echo [Editor] Log : !EDITOR_LOG!
    echo [Editor] Will kill after 20 s...

    :: Use PowerShell to launch with stdout+stderr redirected, then wait 20s, kill
    powershell -NoProfile -Command ^
        "$p = Start-Process -FilePath '%EDITOR_EXE%' -WorkingDirectory '%BIN_DIR%' -RedirectStandardOutput '!EDITOR_LOG!' -RedirectStandardError '!EDITOR_LOG!.err' -PassThru -NoNewWindow; " ^
        "Start-Sleep -Seconds 20; " ^
        "if (-not $p.HasExited) { $p.Kill(); Write-Host '[Editor] Killed after 20s.'; exit 0 } else { Write-Host '[Editor] Process exited early (exit code: ' $p.ExitCode ').'; exit $p.ExitCode }"
    set EDITOR_PS_EXIT=!errorlevel!

    :: Merge stderr file if non-empty
    if exist "!EDITOR_LOG!.err" (
        for %%F in ("!EDITOR_LOG!.err") do if %%~zF GTR 0 (
            echo.                     >> "!EDITOR_LOG!"
            echo --- stderr ---       >> "!EDITOR_LOG!"
            type "!EDITOR_LOG!.err"  >> "!EDITOR_LOG!"
        )
        del "!EDITOR_LOG!.err" >nul 2>&1
    )

    set EDITOR_EXIT=0

    :: Early exit = possible crash
    if !EDITOR_PS_EXIT! NEQ 0 (
        echo [Editor] Process ended early or non-zero exit code detected.
        echo [Editor] Process ended early ^(exit !EDITOR_PS_EXIT!^). >> "%SUMMARY%"
        set EDITOR_EXIT=1
    )

    :: Scan log for crash/exception keywords
    :: Match only spdlog-format lines: [level] ... containing these words
    :: (avoids false positives from Vulkan Loader messages listing layer filenames)
    if exist "!EDITOR_LOG!" (
        findstr /r /i "\[error\].*\(crash\|exception\|fatal\|access.violation\|assert\)" "!EDITOR_LOG!" >nul 2>&1
        if not errorlevel 1 (
            echo [Editor] Crash/exception keyword found in log.
            echo [Editor] Crash/exception keyword found in log. >> "%SUMMARY%"
            findstr /r /i "\[error\].*\(crash\|exception\|fatal\|access.violation\|assert\)" "!EDITOR_LOG!" > "!EDITOR_CRASH!"
            echo [Editor] --- crash lines ---
            type "!EDITOR_CRASH!"
            set EDITOR_EXIT=1
        )
    )

    :: Copy any minidumps from bin dir
    for %%D in ("%BIN_DIR%\*.dmp" "%BIN_DIR%\*.mdmp") do (
        if exist "%%D" (
            echo [Editor] Minidump: %%~nxD  -- saved to logs.
            echo [Editor] Minidump: %%~nxD >> "%SUMMARY%"
            copy "%%D" "%RUN_DIR%\" >nul 2>&1
        )
    )

    if !EDITOR_EXIT! EQU 0 (
        echo [Editor] PASSED  ^(ran 20 s, no crash detected^)
        echo [Editor] PASSED  ^(ran 20 s, no crash detected^) >> "%SUMMARY%"
        set /a PASS_COUNT+=1
    ) else (
        echo [Editor] FAILED  ^(see !EDITOR_LOG!^)
        echo [Editor] FAILED >> "%SUMMARY%"
        set /a FAIL_COUNT+=1
    )
    :editor_done
    echo.
)

:: ============================================================
:: Summary
:: ============================================================
echo ============================================================ >> "%SUMMARY%"
echo  PASSED: %PASS_COUNT%   FAILED: %FAIL_COUNT%                >> "%SUMMARY%"
echo ============================================================ >> "%SUMMARY%"

echo ============================================================
echo  RESULT :  PASSED=%PASS_COUNT%   FAILED=%FAIL_COUNT%
echo  Logs   :  %RUN_DIR%
echo ============================================================
echo.

if %FAIL_COUNT% GTR 0 ( exit /b 1 ) else ( exit /b 0 )
