from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import shutil
import site
import subprocess
import sys
import time
import traceback

from pathlib import Path
from typing import Any


# Resolve the MCP workspace layout relative to this launcher
SCRIPT_DIR = Path(__file__).resolve().parent
WORKSPACE_DIR = SCRIPT_DIR.parent.parent
CACHE_DIR = WORKSPACE_DIR / ".cache" / "mcp"
SITE_PACKAGES_DIR = CACHE_DIR / "site-packages"
PIP_CACHE_DIR = CACHE_DIR / "pip-cache"
LOG_DIR = CACHE_DIR / "logs"
LOCK_PATH = CACHE_DIR / "install.lock"
STAMP_PATH = CACHE_DIR / "install.stamp.json"
BOOTSTRAP_LOG_PATH = LOG_DIR / "bootstrap.log"
REQUIREMENTS_PATH = SCRIPT_DIR / "requirements.lock.txt"
REQUIRED_PACKAGES = ("mcp", "httpx")
LOCK_TIMEOUT_SECONDS = 180.0
STALE_LOCK_SECONDS = 600.0


# Create the runtime cache layout used by the launcher bootstrap flow
def ensure_cache_dirs() -> None:
    CACHE_DIR.mkdir(parents=True, exist_ok=True)
    PIP_CACHE_DIR.mkdir(parents=True, exist_ok=True)
    LOG_DIR.mkdir(parents=True, exist_ok=True)


# Persist detailed bootstrap diagnostics without polluting MCP stdout
def append_log_line(message: str) -> None:
    ensure_cache_dirs()
    timestamp = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime())
    with BOOTSTRAP_LOG_PATH.open("a", encoding="utf-8") as handle:
        handle.write(f"[{timestamp}] {message}\n")


# Keep subprocess Python imports isolated from user-site and custom PYTHONPATH state
def make_subprocess_env() -> dict[str, str]:
    env = os.environ.copy()
    env.pop("PYTHONPATH", None)
    env["PYTHONNOUSERSITE"] = "1"
    return env


# Hash the pinned dependency file so cache invalidation follows dependency edits
def requirements_hash() -> str:
    if not REQUIREMENTS_PATH.exists():
        raise FileNotFoundError(f"Missing requirements file: {REQUIREMENTS_PATH}")

    digest = hashlib.sha256()
    digest.update(REQUIREMENTS_PATH.read_bytes())
    return digest.hexdigest()


# Describe the bootstrap state that the current launcher accepts
def expected_stamp() -> dict[str, Any]:
    return {
        "python_executable": str(Path(sys.executable).resolve()),
        "python_version": list(sys.version_info[:3]),
        "platform": sys.platform,
        "machine": platform.machine(),
        "requirements_sha256": requirements_hash(),
    }


# Read the cached dependency stamp if one exists
def load_stamp() -> dict[str, Any] | None:
    if not STAMP_PATH.exists():
        return None

    try:
        return json.loads(STAMP_PATH.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        append_log_line("install.stamp.json is invalid JSON and will be ignored")
        return None


# Record the dependency snapshot that was just installed successfully
def write_stamp() -> None:
    stamp = expected_stamp()
    stamp["installed_at"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    STAMP_PATH.write_text(json.dumps(stamp, indent=2), encoding="utf-8")


# Compare the cached dependency stamp against the current launcher expectations
def stamp_matches_expected(stamp: dict[str, Any] | None) -> bool:
    if stamp is None:
        return False

    expected = expected_stamp()
    for key, value in expected.items():
        if stamp.get(key) != value:
            return False
    return True


# Add the tool folder and cached site-packages to the current interpreter search path
def configure_import_path() -> None:
    script_dir_str = str(SCRIPT_DIR)
    if script_dir_str not in sys.path:
        sys.path.insert(0, script_dir_str)

    if SITE_PACKAGES_DIR.exists():
        site_packages_str = str(SITE_PACKAGES_DIR)
        if site_packages_str not in sys.path:
            site.addsitedir(site_packages_str)


# Verify the cached packages can really be imported by this vendored Python runtime
def verify_cached_runtime() -> bool:
    if not SITE_PACKAGES_DIR.exists():
        return False

    verify_code = (
        "import site, sys; "
        "site.addsitedir(sys.argv[1]); "
        "import mcp, httpx"
    )
    command = [sys.executable, "-c", verify_code, str(SITE_PACKAGES_DIR)]

    ensure_cache_dirs()
    with BOOTSTRAP_LOG_PATH.open("a", encoding="utf-8") as handle:
        handle.write("\n=== verify cached runtime ===\n")
        handle.write("Command: " + " ".join(command) + "\n")
        result = subprocess.run(
            command,
            cwd=str(WORKSPACE_DIR),
            env=make_subprocess_env(),
            stdout=handle,
            stderr=subprocess.STDOUT,
            check=False,
        )

    return result.returncode == 0


# Decide whether the cached dependency environment can be reused safely
def cache_is_current(force_reinstall: bool) -> bool:
    if force_reinstall:
        return False

    if not SITE_PACKAGES_DIR.exists():
        return False

    if not stamp_matches_expected(load_stamp()):
        return False

    return verify_cached_runtime()


# Detect lock files left behind by interrupted installs
def lock_is_stale() -> bool:
    if not LOCK_PATH.exists():
        return False

    age_seconds = time.time() - LOCK_PATH.stat().st_mtime
    return age_seconds >= STALE_LOCK_SECONDS


# Acquire exclusive ownership of the dependency install flow
def acquire_install_lock() -> None:
    ensure_cache_dirs()

    start_time = time.monotonic()
    while True:
        try:
            fd = os.open(str(LOCK_PATH), os.O_CREAT | os.O_EXCL | os.O_WRONLY)
        except FileExistsError:
            if lock_is_stale():
                append_log_line("Removing stale MCP install lock")
                try:
                    LOCK_PATH.unlink()
                except FileNotFoundError:
                    pass
                continue

            if time.monotonic() - start_time >= LOCK_TIMEOUT_SECONDS:
                raise RuntimeError("Timed out while waiting for the MCP dependency install lock")

            time.sleep(0.25)
            continue

        with os.fdopen(fd, "w", encoding="utf-8") as handle:
            json.dump({"pid": os.getpid(), "created_at": time.time()}, handle)
        return


# Release the install lock when bootstrap completes or fails
def release_install_lock() -> None:
    try:
        LOCK_PATH.unlink()
    except FileNotFoundError:
        pass


# Reset the install target so partial or stale packages do not survive upgrades
def reset_site_packages_dir() -> None:
    if SITE_PACKAGES_DIR.exists():
        shutil.rmtree(SITE_PACKAGES_DIR)
    SITE_PACKAGES_DIR.mkdir(parents=True, exist_ok=True)


# Install pinned dependencies into the workspace-local MCP cache
def install_dependencies() -> None:
    append_log_line("Bootstrapping MCP dependencies")
    STAMP_PATH.unlink(missing_ok=True)
    reset_site_packages_dir()

    command = [
        sys.executable,
        "-m",
        "pip",
        "install",
        "--disable-pip-version-check",
        "--no-input",
        "--upgrade",
        "--target",
        str(SITE_PACKAGES_DIR),
        "--cache-dir",
        str(PIP_CACHE_DIR),
        "-r",
        str(REQUIREMENTS_PATH),
    ]

    ensure_cache_dirs()
    with BOOTSTRAP_LOG_PATH.open("a", encoding="utf-8") as handle:
        handle.write("\n=== bootstrap install ===\n")
        handle.write("Command: " + " ".join(command) + "\n")
        result = subprocess.run(
            command,
            cwd=str(WORKSPACE_DIR),
            env=make_subprocess_env(),
            stdout=handle,
            stderr=subprocess.STDOUT,
            check=False,
        )

    if result.returncode != 0:
        raise RuntimeError("Dependency bootstrap failed")

    if not verify_cached_runtime():
        raise RuntimeError("Dependency bootstrap completed but import verification failed")

    write_stamp()
    append_log_line("MCP dependency bootstrap completed successfully")


# Ensure the cached dependency environment is ready before importing the real server
def ensure_dependencies_ready(force_reinstall: bool) -> None:
    ensure_cache_dirs()
    configure_import_path()

    if cache_is_current(force_reinstall):
        return

    acquire_install_lock()
    try:
        configure_import_path()
        if cache_is_current(False):
            return
        install_dependencies()
    finally:
        release_install_lock()

    configure_import_path()


# Start the real MCP server after launcher bootstrap is complete
def run_server() -> int:
    configure_import_path()
    from server import main as server_main

    return int(server_main() or 0)


# Import the real server module without starting stdio so check mode validates the full stack
def verify_server_import() -> None:
    configure_import_path()
    server_module = __import__("server")
    server_module.mcp._mcp_server.create_initialization_options()


# Emit a short user-facing failure line and keep the full traceback in the bootstrap log
def report_failure(exc: BaseException) -> None:
    ensure_cache_dirs()
    with BOOTSTRAP_LOG_PATH.open("a", encoding="utf-8") as handle:
        handle.write("\n=== launcher failure ===\n")
        traceback.print_exception(exc, file=handle)

    print(
        "MoerEngine MCP launcher failed to prepare dependencies. "
        f"See {BOOTSTRAP_LOG_PATH}",
        file=sys.stderr,
    )


# Parse launcher-only control flags without affecting normal MCP stdio startup
def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="MoerEngine MCP dependency launcher")
    parser.add_argument("--check", action="store_true", help="verify bootstrap and exit")
    parser.add_argument(
        "--bootstrap-only",
        action="store_true",
        help="install or validate cached dependencies and exit",
    )
    parser.add_argument(
        "--force-reinstall",
        action="store_true",
        help="reinstall cached dependencies even if the stamp matches",
    )
    return parser.parse_args(argv)


# Drive the launcher lifecycle and only hand off to MCP once bootstrap is ready
def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)

    try:
        ensure_dependencies_ready(force_reinstall=args.force_reinstall)

        if args.check:
            verify_server_import()
            print("MoerEngine MCP launcher check passed", file=sys.stderr)
            return 0

        if args.bootstrap_only:
            print("MoerEngine MCP dependencies are ready", file=sys.stderr)
            return 0

        return run_server()
    except Exception as exc:  # noqa: BLE001
        report_failure(exc)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())