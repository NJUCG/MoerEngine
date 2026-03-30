# MoerEngine Test Scripts

PowerShell test scripts for quick self-check of MoerEngine executables.
Each script is self-contained, targeting one executable, and shares logic via `common.ps1`.

## Requirements

- PowerShell 5.1+
- Built executables under `target/bin/<Config>/`

## Scripts

| Script | Executable | Description |
|--------|-----------|-------------|
| `test_rhi_translate.ps1` | `TestRHITranslate.exe` | RHI multi-queue translate tests (synchronous, exit-code checked) |
| `test_editor.ps1` | `MoerEditor.exe` | Launch editor, run for N seconds, then kill and inspect log |
| `test_all.ps1` | both | Run all tests in sequence, single summary |
| `common.ps1` | — | Shared library, dot-sourced by all scripts above |

## Usage

Run from the repo root (or any directory — scripts use `$PSScriptRoot` internally):

```powershell
# All tests, Debug build (default)
.\testscripts\test_all.ps1

# Single test
.\testscripts\test_rhi_translate.ps1
.\testscripts\test_editor.ps1

# Choose build config
.\testscripts\test_all.ps1 -Config Release
.\testscripts\test_all.ps1 -Config RelWithDebInfo

# Pass extra arguments to the executable (repeatable)
.\testscripts\test_all.ps1 -ExtraArgs @("-DENABLE_VALIDATION=1", "-DDEBUG_QUEUES=1")

# Custom editor timeout (default 20 s)
.\testscripts\test_editor.ps1 -TimeoutSec 30
```

If PowerShell execution policy blocks the scripts, run once with:

```powershell
powershell -ExecutionPolicy Bypass -File .\testscripts\test_all.ps1
```

## Output

Each run creates a timestamped folder under `logs/`:

```
logs/
└── run_YYYYMMDD_HHMMSS/
    ├── summary.txt                 ← PASSED/FAILED overview for the run
    ├── rhi_translate.log           ← Full stdout+stderr of TestRHITranslate
    ├── moereditor.log              ← Full stdout+stderr of MoerEditor
    ├── moereditor_crash.txt        ← Lines that triggered error detection (if any)
    └── *.dmp / *.mdmp              ← Minidumps copied from bin dir (if any)
```

`summary.txt` always exists after a run and contains the final PASSED/FAILED counts.

## Error Detection

The editor test (`test_editor.ps1`, `test_all.ps1`) reports **FAILED** when any of the following are found in the log:

| Pattern | Source |
|---------|--------|
| `[error] [VulkanDebugCallback.cpp:…]` | Any message routed through the Vulkan debug callback |
| `Validation Error: [ VUID-… ]` | Vulkan validation layer hard error |
| `Validation Warning: [ VUID-… ]` | Vulkan validation layer warning |
| `[VUID-…]` detail lines | Follow-up context lines emitted by the validation layer |
| `[error] … crash / exception / fatal / assert` | Application-level fatal errors |

Vulkan Loader info messages (layer discovery, driver enumeration) are **excluded** — they appear at `[error]` severity in the spdlog output due to the debug callback routing, but do not indicate real errors.

The RHI test (`test_rhi_translate.ps1`) relies on the executable's exit code (0 = all pass).

## Exit Codes

| Code | Meaning |
|------|---------|
| `0` | All tests passed |
| `1` | One or more tests failed |

## Adding a New Test

1. Create `testscripts/test_<name>.ps1`.
2. Add the standard header:
   ```powershell
   . "$PSScriptRoot\common.ps1"
   Initialize-TestRun -Config $Config -ScriptDir $PSScriptRoot
   ```
3. Use `Invoke-ExeSync` or `Invoke-ExeTimed` to run your executable.
4. Use `Test-LogForErrors` to scan the log, `Register-Pass`/`Register-Fail` to record the result.
5. Call `Finish-TestRun` at the end.
6. Add the new test block to `test_all.ps1`.
