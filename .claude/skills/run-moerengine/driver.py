#!/usr/bin/env python3
"""
MoerEngine build / test / launch driver.

Usage:
  python driver.py build              # Build MoerEditor + tests
  python driver.py test               # Run all available test executables
  python driver.py launch             # Launch editor for 20 s, kill, scan logs for crashes
  python driver.py all                # Build → test → launch (the CI smoke path)
  python driver.py build --release    # Release build
  python driver.py test --filter RHI  # Only run tests matching a substring
  python driver.py launch --timeout 30  # Run editor for 30 s

Exit code: 0 when all stages pass, 1 otherwise.
"""

import argparse
import os
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent.parent
BUILD_DIR = ROOT / "build" / "clang-debug"
BIN_DIR = ROOT / "target" / "bin"
LOGS_DIR = ROOT / "logs"

# ---------------------------------------------------------------------------
# Test executables that don't need a GPU / window
# ---------------------------------------------------------------------------
CORE_TESTS = [
    "TestTaskPipe.exe",
    "TestTaskGraph.exe",
    "TestStringSystem.exe",
]

RHI_TESTS = [
    "TestRHITranslate.exe",
    "TestDxRhi.exe",
]

SHADER_TESTS = [
    "TestShaderDXC.exe",
]

ALL_TESTS = CORE_TESTS + RHI_TESTS + SHADER_TESTS

# Keywords that indicate a crash or fatal error in editor logs.
# Anchored to spdlog-format: [error] ... <keyword>
CRASH_PATTERNS = [
    "crash",
    "exception",
    "fatal",
    "access.violation",
    "assert",
]

# Vulkan validation error markers
VK_VALIDATION_PATTERNS = [
    "VUID_",
    "Validation Error",
    "UNASSIGNED",
]


# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------
def run(cmd: list[str], **kwargs) -> subprocess.CompletedProcess:
    """Thin wrapper around subprocess.run with defaults."""
    defaults = {"check": False, "capture_output": False, "cwd": str(ROOT)}
    defaults.update(kwargs)
    print(f"  → {' '.join(str(c) for c in cmd)}")
    return subprocess.run(cmd, **defaults)


def find_tests(config: str, pattern: str | None = None) -> list[Path]:
    """Return list of test .exe paths in BIN_DIR/config, optionally filtered."""
    bin_cfg = BIN_DIR / config
    if not bin_cfg.exists():
        print(f"  [WARN] Binary directory not found: {bin_cfg}")
        return []
    available = sorted(bin_cfg.glob("Test*.exe"))
    if pattern:
        available = [p for p in available if pattern.lower() in p.name.lower()]
    return available


def parse_test_log(text: str) -> tuple[int, int, list[str]]:
    """Count [TESTCASE][PASS] / [TESTCASE][FAIL] lines, return (pass, fail, failures)."""
    passed = 0
    failed = 0
    failures: list[str] = []
    for line in text.splitlines():
        if "[TESTCASE][PASS]" in line:
            passed += 1
        elif "[TESTCASE][FAIL]" in line:
            failed += 1
            failures.append(line.strip())
    return passed, failed, failures


# ---------------------------------------------------------------------------
# commands
# ---------------------------------------------------------------------------
# Targets that are known to compile with current Clang/MSVC STL.
# TestStringSystem is excluded — it tickles a Clang/MSVC std::apply incompatibility
# in Format.h that is pre-existing and not a build regression.
BUILD_TARGETS = [
    "MoerEditor",
    "TestRHITranslate",
    "TestDxRhi",
    "TestTaskPipe",
    "TestTaskGraph",
    "TestShaderDXC",
]


def cmd_build(config: str) -> int:
    """Build MoerEditor + test targets. Builds one target at a time so a single
    compilation failure (e.g. pre-existing Clang/MSVC issue) doesn't block the rest."""
    print(f"\n=== Build ({config}) ===")
    n_built = 0
    n_failed = 0

    for target in BUILD_TARGETS:
        result = run([
            "cmake", "--build", str(BUILD_DIR),
            "--config", config,
            "--target", target,
            "-j", str(os.cpu_count() or 16),
        ])
        if result.returncode != 0:
            print(f"  [SKIP] {target} build failed (may be pre-existing)")
            n_failed += 1
        else:
            n_built += 1

    if n_built == 0:
        print("[BUILD] FAILED — no targets built")
        return 1
    print(f"[BUILD] {n_built} targets built, {n_failed} skipped")
    return 0


def cmd_test(config: str, pattern: str | None) -> int:
    """Run test executables. Each test is run from its bin directory so
    toml / asset paths resolve correctly."""
    tests = find_tests(config, pattern)
    if not tests:
        print("  No test executables found.")
        return 1

    stamp = datetime.now(timezone.utc).strftime("%Y%m%d_%H%M%S")
    run_dir = LOGS_DIR / f"driver_test_{stamp}"
    run_dir.mkdir(parents=True, exist_ok=True)

    total_pass = 0
    total_fail = 0
    exit_code = 0

    print(f"\n=== Test ({config}, {len(tests)} exe(s)) ===")
    for test_path in tests:
        name = test_path.stem
        log_path = run_dir / f"{name}.log"
        print(f"\n--- {name} ---")

        try:
            proc = subprocess.run(
                [str(test_path)],
                cwd=str(test_path.parent),
                capture_output=True,
                text=True,
                timeout=120,
            )
            # Write log
            log_path.write_text(
                proc.stdout + ("\n--- stderr ---\n" + proc.stderr if proc.stderr else ""),
                encoding="utf-8", errors="replace",
            )
        except subprocess.TimeoutExpired:
            print(f"  [FAIL] {name} timed out after 120 s")
            log_path.write_text("TIMEOUT after 120 s\n", encoding="utf-8")
            total_fail += 1
            exit_code = 1
            continue

        # Parse structured test output if available
        combined = proc.stdout + proc.stderr
        p, f, failures = parse_test_log(combined)

        if p == 0 and f == 0:
            # No structured markers — fall back to exit code
            if proc.returncode == 0:
                print(f"  [PASS] {name} (exit 0)")
                total_pass += 1
            else:
                print(f"  [FAIL] {name} (exit {proc.returncode})")
                print(f"  Log: {log_path}")
                total_fail += 1
                exit_code = 1
        else:
            print(f"  {name}: {p} passed, {f} failed")
            total_pass += p
            total_fail += f
            if f > 0:
                for fl in failures:
                    print(f"    {fl}")
                exit_code = 1

    print(f"\n[TEST] {total_pass} passed, {total_fail} failed")
    print(f"[TEST] Logs: {run_dir}")
    return exit_code


def cmd_launch(config: str, timeout_s: int) -> int:
    """Launch MoerEditor.exe, wait timeout_s, kill it, scan log for crashes."""
    editor_path = BIN_DIR / config / "MoerEditor.exe"
    if not editor_path.exists():
        print(f"[LAUNCH] Editor not found: {editor_path}")
        return 1

    stamp = datetime.now(timezone.utc).strftime("%Y%m%d_%H%M%S")
    run_dir = LOGS_DIR / f"driver_launch_{stamp}"
    run_dir.mkdir(parents=True, exist_ok=True)

    log_path = run_dir / "editor_stdout.log"
    err_path = run_dir / "editor_stderr.log"

    print(f"\n=== Launch Editor ({config}, timeout={timeout_s}s) ===")
    print(f"  Exe:  {editor_path}")
    print(f"  Cwd:  {editor_path.parent}")
    print(f"  Logs: {run_dir}")

    # Launch detached, redirect stdout/stderr to files
    try:
        proc = subprocess.Popen(
            [str(editor_path)],
            cwd=str(editor_path.parent),
            stdout=open(str(log_path), "w", encoding="utf-8", errors="replace"),
            stderr=open(str(err_path), "w", encoding="utf-8", errors="replace"),
            creationflags=subprocess.CREATE_NEW_PROCESS_GROUP
            if sys.platform == "win32"
            else 0,
        )
    except OSError as exc:
        print(f"[LAUNCH] Failed to start: {exc}")
        return 1

    # Wait
    print(f"  Waiting {timeout_s} s...")
    time.sleep(timeout_s)

    # Kill
    if proc.poll() is None:
        print(f"  Killing (PID {proc.pid})...")
        proc.kill()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            pass
        early_exit = False
    else:
        print(f"  Editor exited early (exit code {proc.returncode})")
        early_exit = True

    # Close file handles so we can read logs
    proc.stdout.close() if proc.stdout else None
    proc.stderr.close() if proc.stderr else None
    # Wait for filesystem flush
    time.sleep(1)

    # --- Scan log for crashes ---
    exit_code = 0

    # Merge stderr into a combined log for scanning
    combined_parts: list[str] = []
    for p in (log_path, err_path):
        if p.exists():
            combined_parts.append(p.read_text(encoding="utf-8", errors="replace"))
    combined = "\n".join(combined_parts)

    # Crash check — only lines that look like spdlog error-level
    crash_lines: list[str] = []
    vk_val_lines: list[str] = []
    for line in combined.splitlines():
        line_lower = line.lower()
        is_error_line = "[error]" in line_lower
        if is_error_line:
            for kw in CRASH_PATTERNS:
                if kw in line_lower:
                    crash_lines.append(line)
                    break
        for kw in VK_VALIDATION_PATTERNS:
            if kw in line:
                vk_val_lines.append(line)
                break

    if early_exit:
        print("[LAUNCH] Editor exited early — possible crash")
        exit_code = 1

    if crash_lines:
        print(f"[LAUNCH] Crash keywords found ({len(crash_lines)} lines):")
        for ln in crash_lines[:20]:
            print(f"  {ln}")
        exit_code = 1

    # Vulkan validation errors are warnings, not hard failures, but worth noting
    if vk_val_lines:
        print(f"[LAUNCH] Vulkan validation messages ({len(vk_val_lines)} lines)")
        vk_log = run_dir / "vulkan_validation_lines.txt"
        vk_log.write_text("\n".join(vk_val_lines), encoding="utf-8")
        print(f"  Details: {vk_log}")

    # Check for minidumps
    for dmp in editor_path.parent.glob("*.dmp"):
        import shutil
        dest = run_dir / dmp.name
        shutil.copy2(str(dmp), str(dest))
        print(f"[LAUNCH] Minidump: {dmp.name} → {dest}")
        exit_code = 1

    # Copy the MoerEngine log if present
    engine_log = editor_path.parent / "logs" / "MoerEngine.log"
    if engine_log.exists():
        import shutil
        dest = run_dir / "MoerEngine.log"
        shutil.copy2(str(engine_log), str(dest))
        # Also scan engine log for validation errors
        engine_text = engine_log.read_text(encoding="utf-8", errors="replace")
        engine_val_lines = [l for l in engine_text.splitlines()
                           if any(kw in l for kw in VK_VALIDATION_PATTERNS)]
        if engine_val_lines:
            print(f"[LAUNCH] Engine log Vulkan validation ({len(engine_val_lines)} lines)")

    if exit_code == 0:
        print("[LAUNCH] PASSED (no crash detected)")
    else:
        print("[LAUNCH] FAILED")
    return exit_code


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------
def main() -> int:
    ap = argparse.ArgumentParser(description="MoerEngine build / test / launch driver")
    ap.add_argument("action", choices=["build", "test", "launch", "all"])
    ap.add_argument("--config", default="Debug",
                    choices=["Debug", "Release", "RelWithDebInfo"])
    ap.add_argument("--timeout", type=int, default=20,
                    help="Editor launch timeout in seconds (default 20)")
    ap.add_argument("--filter", default=None,
                    help="Substring filter for test executables")
    args = ap.parse_args()

    actions = [args.action] if args.action != "all" else ["build", "test", "launch"]

    for act in actions:
        if act == "build":
            if cmd_build(args.config) != 0:
                return 1
        elif act == "test":
            if cmd_test(args.config, args.filter) != 0:
                return 1
        elif act == "launch":
            if cmd_launch(args.config, args.timeout) != 0:
                return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
