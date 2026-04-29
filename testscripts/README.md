# MoerEngine Test Scripts

The test entrypoints are organized by platform.
Windows PowerShell and Linux shell entrypoints do not share one flat directory anymore.

## Layout

| Directory | Platform | Notes |
|-----------|----------|-------|
| `windows/` | Windows | Current primary validation flow |
| `linux/` | Linux | Linux-specific validation entrypoints |

## Windows Scripts

Requirements:
- PowerShell 5.1+
- Built executables under `target/bin/<Config>/`

| Script | Executable | Description |
|--------|-----------|-------------|
| `windows/test_taskgraph.ps1` | `TestTaskGraph.exe`, `TestTaskPipe.exe` | TaskGraph / TaskPipe regression suite with structured testcase parsing |
| `windows/test_rhi_translate.ps1` | `TestRHITranslate.exe` | RHI multi-queue translate tests with descriptor-heap capability probe and validation blocker parsing |
| `windows/test_editor.ps1` | `MoerEditor.exe` | Launch editor, run for N seconds, then kill and inspect the log |
| `windows/test_all.ps1` | multiple executables | Run the Windows validation suite in sequence, single summary |
| `windows/common.ps1` | — | Shared PowerShell library, dot-sourced by the Windows scripts |

Usage from the repo root:

```powershell
.\testscripts\windows\test_all.ps1
.\testscripts\windows\test_rhi_translate.ps1
.\testscripts\windows\test_taskgraph.ps1
.\testscripts\windows\test_editor.ps1

.\testscripts\windows\test_all.ps1 -Config Release
.\testscripts\windows\test_all.ps1 -Config RelWithDebInfo
.\testscripts\windows\test_all.ps1 -ExtraArgs @("-DENABLE_VALIDATION=1", "-DDEBUG_QUEUES=1")
.\testscripts\windows\test_editor.ps1 -TimeoutSec 30
```

If PowerShell execution policy blocks the scripts:

```powershell
powershell -ExecutionPolicy Bypass -File .\testscripts\windows\test_all.ps1
```

## Linux Scripts

| Script | Executable | Description |
|--------|-----------|-------------|
| `linux/test_taskgraph.sh` | `TestTaskGraph`, `TestTaskPipe`, `TestStringSystem` | Linux core-only configure/build/run path for taskgraph and string-system regression |
| `linux/test_string_system.sh` | `TestStringSystem` | Linux core-only configure/build/run path for string-system regression |

Usage from the repo root:

```bash
./testscripts/linux/test_taskgraph.sh
./testscripts/linux/test_taskgraph.sh Release
./testscripts/linux/test_string_system.sh
./testscripts/linux/test_string_system.sh Release
```

## Output

Each run creates a timestamped folder under `logs/`:

```
logs/
└── run_YYYYMMDD_HHMMSS/
    ├── summary.txt                 ← PASSED/FAILED/SKIPPED overview for the run
    ├── taskgraph.log               ← Full stdout+stderr of TestTaskGraph
    ├── taskpipe.log                ← Full stdout+stderr of TestTaskPipe
    ├── rhi_translate.log           ← Full stdout+stderr of TestRHITranslate
    ├── moereditor.log              ← Full stdout+stderr of MoerEditor
    ├── moereditor_crash.txt        ← Lines that triggered error detection (if any)
    └── *.dmp / *.mdmp              ← Minidumps copied from bin dir (if any)
```

`summary.txt` always exists after a run and contains both top-level test counts and grouped subtest counts with PASSED/FAILED/SKIPPED states.

## Error Detection

The Windows editor test (`windows/test_editor.ps1`, `windows/test_all.ps1`) reports **FAILED** when any of the following are found in the log:

| Pattern | Source |
|---------|--------|
| `[error] [VulkanDebugCallback.cpp:…]` | Any message routed through the Vulkan debug callback |
| `Validation Error: [ VUID-… ]` | Vulkan validation layer hard error |
| `Validation Warning: [ VUID-… ]` | Vulkan validation layer warning |
| `[VUID-…]` detail lines | Follow-up context lines emitted by the validation layer |
| `[error] … crash / exception / fatal / assert` | Application-level fatal errors |

Vulkan Loader info messages (layer discovery, driver enumeration) are **excluded** — they appear at `[error]` severity in the spdlog output due to the debug callback routing, but do not indicate real errors.

The Windows RHI test (`windows/test_rhi_translate.ps1`) also treats Vulkan validation errors as blockers and records descriptor-heap capability as a grouped subtest.

## RenderGraph / RHI Focused Flow

Use the Windows RHI script for focused RenderGraph and RHI command-list changes:

```powershell
.\testscripts\windows\test_rhi_translate.ps1
.\testscripts\windows\test_rhi_translate.ps1 -Config Release
```

The script resolves the configured build directory, builds `TestRHITranslate`, runs `target/bin/<Config>/TestRHITranslate.exe`, scans the log for Vulkan validation blockers, parses every `[TESTCASE][PASS|FAIL|SKIP]` marker, and requires the `RHICommandListRGBaseline` marker to be present. Logs and `summary.txt` are written to `logs/run_YYYYMMDD_HHMMSS/`.

After a milestone-sized RenderGraph landing, run the aggregate Windows flow:

```powershell
.\testscripts\windows\test_all.ps1
```

## Exit Codes

| Code | Meaning |
|------|---------|
| `0` | All top-level tests passed |
| `1` | One or more top-level tests failed |

## Adding a New Test

1. Choose the owning platform directory first.
2. For Windows, create `testscripts/windows/test_<name>.ps1`.
3. For Linux, create `testscripts/linux/test_<name>.sh` only when a real Linux-only validation path exists.
4. Windows scripts should add the standard header:
   ```powershell
   . "$PSScriptRoot\common.ps1"
   Initialize-TestRun -Config $Config -ScriptDir $PSScriptRoot
   ```
5. Windows scripts should use `Invoke-ExeSync` or `Invoke-ExeTimed` to run the executable.
6. Use `Test-LogForErrors` to scan the log, `Register-Pass`/`Register-Fail`/`Register-Skip` to record the result, and `Register-Subtest` when grouped detail is needed.
7. Call `Finish-TestRun` at the end.
8. If the new check belongs to the Windows aggregate flow, add it to `testscripts/windows/test_all.ps1`.
