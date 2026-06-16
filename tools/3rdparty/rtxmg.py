# RTXMG是CLAS拓展的Sample，而不是SDK，所以不应该直接接入。此处RTXMG相关代码暂时废弃

from __future__ import annotations

import subprocess
import sys

from pathlib import Path


RTXMG_REPOSITORY_URL = "https://github.com/NVIDIA-RTX/RTXMG.git"
RTXMG_TAG = "v1.0.1"
RTXMG_SUBMODULES = ("extern/donut", "extern/osd_lite")
SCRIPT_NAME = Path(__file__).name
SCRIPT_LOG_SEPARATOR = "/" * 72


# Resolve the workspace root from tools/3rdparty/rtxmg.py
SCRIPT_DIR = Path(__file__).resolve().parent
WORKSPACE_DIR = SCRIPT_DIR.parent.parent
RTXMG_DEST_DIR = WORKSPACE_DIR / "3rdparty" / "rtxmg"


# Print a clear script-level boundary so this output does not blend into CMake logs
def print_script_marker(marker: str, exit_code: int | None = None) -> None:
    print()
    print(SCRIPT_LOG_SEPARATOR)
    print(f"// Python Script {marker}: {SCRIPT_NAME}")
    if exit_code is not None:
        print(f"// Exit Code: {exit_code}")
    print(SCRIPT_LOG_SEPARATOR)
    print()


# Execute a git command and stream output directly to the current console
def run_command(command: list[str], cwd: Path | None = None) -> None:
    location = cwd if cwd is not None else WORKSPACE_DIR
    print(f"[rtxmg] cwd: {location}")
    print(f"[rtxmg] run: {' '.join(command)}")

    subprocess.run(
        command,
        cwd=location,
        check=True,
    )


# Fail early if the destination already contains files, to avoid mixing states
def validate_destination() -> None:
    if not RTXMG_DEST_DIR.exists():
        return

    if any(RTXMG_DEST_DIR.iterdir()):
        raise RuntimeError(
            f"Destination already exists and is not empty: {RTXMG_DEST_DIR}"
        )


# Verify that git is available before starting the clone flow
def ensure_git_available() -> None:
    try:
        run_command(["git", "--version"])
    except FileNotFoundError as exc:
        raise RuntimeError("git is not available in PATH") from exc


# Clone RTXMG with the minimal submodule set discussed for MoerEngine
def main() -> int:
    exit_code = 0
    print_script_marker("Begin")

    try:
        ensure_git_available()

        RTXMG_DEST_DIR.parent.mkdir(parents=True, exist_ok=True)
        validate_destination()

        run_command(
            [
                "git",
                "clone",
                "--depth",
                "1",
                "--branch",
                RTXMG_TAG,
                RTXMG_REPOSITORY_URL,
                str(RTXMG_DEST_DIR),
            ]
        )

        run_command(
            [
                "git",
                "submodule",
                "update",
                "--init",
                "--recursive",
                "--depth",
                "1",
                *RTXMG_SUBMODULES,
            ],
            cwd=RTXMG_DEST_DIR,
        )
    except subprocess.CalledProcessError as exc:
        print(f"[rtxmg] command failed with exit code {exc.returncode}", file=sys.stderr)
        exit_code = exc.returncode
    except RuntimeError as exc:
        print(f"[rtxmg] {exc}", file=sys.stderr)
        exit_code = 1
    else:
        print(f"[rtxmg] cloned RTXMG {RTXMG_TAG} into {RTXMG_DEST_DIR}")
    finally:
        print_script_marker("End", exit_code)

    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())