from __future__ import annotations

import argparse
import ctypes
import json
import os
import re
import struct
import subprocess
import sys
import time
from ctypes import wintypes
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))

from tools.threading.runtime_verify import (  # noqa: E402
    DEFAULT_FORBIDDEN_PATTERNS,
    capture_print_window,
    close_process,
)


SPLASH_CLASS = "MoerEngineStartupSplashWindow"
SPLASH_TITLE = "MoerEngine Startup"
MAIN_TITLE = "MoerEditor"
MONITOR_DEFAULTTOPRIMARY = 1
WM_NCHITTEST = 0x0084
HTCLIENT = 1
FIRST_PRESENT_LOG = "[Startup][Engine] First main-window present submitted on the Game Thread."
CONTENT_READY_LOG = "[Startup][Renderer] First-present receipt armed: scene_content_ready=true."


class MonitorInfo(ctypes.Structure):
    _fields_ = [
        ("cbSize", wintypes.DWORD),
        ("rcMonitor", wintypes.RECT),
        ("rcWork", wintypes.RECT),
        ("dwFlags", wintypes.DWORD),
    ]


if os.name == "nt":
    user32 = ctypes.windll.user32
    user32.MonitorFromPoint.argtypes = (wintypes.POINT, wintypes.DWORD)
    user32.MonitorFromPoint.restype = wintypes.HANDLE
    user32.GetMonitorInfoW.argtypes = (wintypes.HANDLE, ctypes.POINTER(MonitorInfo))
    user32.GetMonitorInfoW.restype = wintypes.BOOL
    user32.SendMessageW.argtypes = (
        wintypes.HWND,
        wintypes.UINT,
        wintypes.WPARAM,
        wintypes.LPARAM,
    )
    user32.SendMessageW.restype = ctypes.c_ssize_t
    try:
        # DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2. Without this, Win32
        # virtualizes a 150% splash from 1020x570 to 680x380 and PrintWindow
        # captures only the upper-left logical area.
        user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
    except (AttributeError, OSError):
        user32.SetProcessDPIAware()
else:
    user32 = None


def window_text(hwnd: int) -> str:
    length = user32.GetWindowTextLengthW(hwnd)
    buffer = ctypes.create_unicode_buffer(max(1, length + 1))
    user32.GetWindowTextW(hwnd, buffer, len(buffer))
    return buffer.value


def window_class(hwnd: int) -> str:
    buffer = ctypes.create_unicode_buffer(256)
    user32.GetClassNameW(hwnd, buffer, len(buffer))
    return buffer.value


def splash_placement(hwnd: int) -> dict[str, object]:
    window_rect = wintypes.RECT()
    if not user32.GetWindowRect(hwnd, ctypes.byref(window_rect)):
        raise ctypes.WinError()

    monitor = user32.MonitorFromPoint(wintypes.POINT(0, 0), MONITOR_DEFAULTTOPRIMARY)
    monitor_info = MonitorInfo(cbSize=ctypes.sizeof(MonitorInfo))
    if not monitor or not user32.GetMonitorInfoW(monitor, ctypes.byref(monitor_info)):
        raise ctypes.WinError()

    window_center_x = (window_rect.left + window_rect.right) // 2
    window_center_y = (window_rect.top + window_rect.bottom) // 2
    work_center_x = (monitor_info.rcWork.left + monitor_info.rcWork.right) // 2
    work_center_y = (monitor_info.rcWork.top + monitor_info.rcWork.bottom) // 2
    hit_test_lparam = (window_center_x & 0xFFFF) | ((window_center_y & 0xFFFF) << 16)

    return {
        "window_rect": [
            window_rect.left,
            window_rect.top,
            window_rect.right,
            window_rect.bottom,
        ],
        "primary_work_area": [
            monitor_info.rcWork.left,
            monitor_info.rcWork.top,
            monitor_info.rcWork.right,
            monitor_info.rcWork.bottom,
        ],
        "center_offset": [window_center_x - work_center_x, window_center_y - work_center_y],
        "hit_test": int(user32.SendMessageW(hwnd, WM_NCHITTEST, 0, hit_test_lparam)),
    }


def visible_windows(pid: int) -> list[dict[str, object]]:
    windows: list[dict[str, object]] = []
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    def callback(hwnd: int, _: int) -> bool:
        window_pid = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(window_pid))
        if window_pid.value == pid and user32.IsWindowVisible(hwnd):
            windows.append(
                {
                    "hwnd": int(hwnd),
                    "title": window_text(hwnd),
                    "class": window_class(hwnd),
                    "hung": bool(user32.IsHungAppWindow(hwnd)),
                }
            )
        return True

    callback_ref = callback_type(callback)
    user32.EnumWindows(callback_ref, 0)
    return windows


def find_window(
    windows: list[dict[str, object]], *, title: str | None = None, class_name: str | None = None
) -> int:
    for window in windows:
        if title is not None and window["title"] != title:
            continue
        if class_name is not None and window["class"] != class_name:
            continue
        return int(window["hwnd"])
    return 0


def white_ratio(path: Path) -> float:
    data = path.read_bytes()
    if len(data) < 54 or data[:2] != b"BM":
        raise RuntimeError(f"Not a BMP capture: {path}")
    pixel_offset = struct.unpack_from("<I", data, 10)[0]
    width = abs(struct.unpack_from("<i", data, 18)[0])
    height = abs(struct.unpack_from("<i", data, 22)[0])
    bit_count = struct.unpack_from("<H", data, 28)[0]
    if bit_count != 32:
        raise RuntimeError(f"Expected a 32-bit BMP capture, got {bit_count}: {path}")

    pixel_count = width * height
    stride = max(1, pixel_count // 200_000)
    white = 0
    sampled = 0
    for pixel_index in range(0, pixel_count, stride):
        offset = pixel_offset + pixel_index * 4
        blue, green, red = data[offset : offset + 3]
        white += int(red >= 245 and green >= 245 and blue >= 245)
        sampled += 1
    return white / max(sampled, 1)


def placeholder_gray_ratio(path: Path) -> float:
    data = path.read_bytes()
    if len(data) < 54 or data[:2] != b"BM":
        raise RuntimeError(f"Not a BMP capture: {path}")
    pixel_offset = struct.unpack_from("<I", data, 10)[0]
    width = abs(struct.unpack_from("<i", data, 18)[0])
    height = abs(struct.unpack_from("<i", data, 22)[0])
    bit_count = struct.unpack_from("<H", data, 28)[0]
    if bit_count != 32:
        raise RuntimeError(f"Expected a 32-bit BMP capture, got {bit_count}: {path}")

    pixel_count = width * height
    stride = max(1, pixel_count // 200_000)
    placeholder_gray = 0
    sampled = 0
    for pixel_index in range(0, pixel_count, stride):
        offset = pixel_offset + pixel_index * 4
        blue, green, red = data[offset : offset + 3]
        neutral = max(red, green, blue) - min(red, green, blue) <= 4
        placeholder_gray += int(neutral and 105 <= red <= 165)
        sampled += 1
    return placeholder_gray / max(sampled, 1)


def scan_forbidden(stdout_path: Path, stderr_path: Path) -> list[str]:
    matches: list[str] = []
    patterns = [re.compile(pattern, re.IGNORECASE) for pattern in DEFAULT_FORBIDDEN_PATTERNS]
    for path in (stdout_path, stderr_path):
        if not path.exists():
            continue
        for line_number, line in enumerate(
            path.read_text(encoding="utf-8", errors="replace").splitlines(), start=1
        ):
            if any(pattern.search(line) for pattern in patterns):
                matches.append(f"{path.name}:{line_number}: {line}")
    return matches


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Launch MoerEditor and verify the native startup splash, hidden main window, "
            "first-present handoff, screenshots, responsiveness, and shutdown logs."
        )
    )
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--workdir", type=Path, required=True)
    parser.add_argument("--outdir", type=Path, required=True)
    parser.add_argument("--config", type=Path)
    parser.add_argument("--splash-timeout", type=float, default=8.0)
    parser.add_argument("--startup-timeout", type=float, default=180.0)
    parser.add_argument("--handoff-timeout", type=float, default=5.0)
    parser.add_argument("--close-timeout", type=float, default=45.0)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if os.name != "nt":
        print("verify_startup_splash.py currently supports Windows only.", file=sys.stderr)
        return 2

    exe = args.exe.resolve()
    workdir = args.workdir.resolve()
    outdir = args.outdir.resolve()
    config = args.config.resolve() if args.config else None
    if not exe.is_file():
        print(f"MoerEditor executable does not exist: {exe}", file=sys.stderr)
        return 2
    if not workdir.is_dir():
        print(f"Working directory does not exist: {workdir}", file=sys.stderr)
        return 2
    if config is not None and not config.is_file():
        print(f"Config does not exist: {config}", file=sys.stderr)
        return 2

    outdir.mkdir(parents=True, exist_ok=True)
    stdout_path = outdir / "stdout.log"
    stderr_path = outdir / "stderr.log"
    report_path = outdir / "report.json"
    command = [str(exe)]
    if config is not None:
        command.extend(["--config", str(config)])

    report: dict[str, object] = {
        "command": command,
        "workdir": str(workdir),
        "config": str(config) if config else None,
        "timeline": [],
        "captures": [],
        "failure_reasons": [],
        "success": False,
    }
    failures: list[str] = report["failure_reasons"]
    started = time.monotonic()
    process: subprocess.Popen[bytes] | None = None
    splash_hwnd = 0
    main_hwnd = 0

    def record(event: str, windows: list[dict[str, object]]) -> None:
        report["timeline"].append(
            {
                "seconds": time.monotonic() - started,
                "event": event,
                "windows": windows,
            }
        )

    try:
        with stdout_path.open("wb") as stdout, stderr_path.open("wb") as stderr:
            process = subprocess.Popen(command, cwd=workdir, stdout=stdout, stderr=stderr)
            report["pid"] = process.pid

            splash_deadline = time.monotonic() + args.splash_timeout
            while time.monotonic() < splash_deadline:
                if process.poll() is not None:
                    raise RuntimeError(f"MoerEditor exited before showing Splash: {process.returncode}")
                windows = visible_windows(process.pid)
                splash_hwnd = find_window(windows, title=SPLASH_TITLE, class_name=SPLASH_CLASS)
                early_main = find_window(windows, title=MAIN_TITLE)
                if early_main:
                    failures.append("The main window became visible before the Splash was observed")
                if splash_hwnd:
                    record("splash_visible", windows)
                    break
                time.sleep(0.05)
            if not splash_hwnd:
                raise RuntimeError("Startup Splash window was not found")

            # The native splash intentionally fades in. Capture after a few
            # animation ticks so PrintWindow does not sample opacity zero.
            time.sleep(0.5)
            splash_capture_path = outdir / "01_splash.bmp"
            splash_capture = capture_print_window(splash_hwnd, splash_capture_path)
            splash_capture["white_ratio"] = white_ratio(splash_capture_path)
            report["captures"].append(splash_capture)

            placement = splash_placement(splash_hwnd)
            report["splash_placement"] = placement
            center_offset = placement["center_offset"]
            if abs(center_offset[0]) > 1 or abs(center_offset[1]) > 1:
                failures.append(
                    "Splash is not centered in the primary monitor work area: "
                    f"offset={center_offset}"
                )
            if placement["hit_test"] != HTCLIENT:
                failures.append(
                    "Splash client area is draggable: "
                    f"WM_NCHITTEST returned {placement['hit_test']}"
                )

            startup_deadline = started + args.startup_timeout
            hung_samples = 0
            samples = 0
            while time.monotonic() < startup_deadline:
                if process.poll() is not None:
                    raise RuntimeError(f"MoerEditor exited during startup: {process.returncode}")
                windows = visible_windows(process.pid)
                samples += 1
                if splash_hwnd and user32.IsWindow(splash_hwnd):
                    hung_samples += int(bool(user32.IsHungAppWindow(splash_hwnd)))
                main_hwnd = find_window(windows, title=MAIN_TITLE)
                if main_hwnd:
                    record("main_window_visible", windows)
                    break
                time.sleep(0.1)
            report["splash_responsiveness_samples"] = samples
            report["splash_hung_samples"] = hung_samples
            if hung_samples:
                failures.append(f"Splash was reported hung in {hung_samples}/{samples} samples")
            if not main_hwnd:
                raise RuntimeError("Main window did not become visible before startup timeout")

            # Capture the handoff immediately and during its first half-second.
            # A delayed-only screenshot can miss a transient blank/white frame
            # between ShowMainWindow and the first stable compositor update.
            handoff_started = time.monotonic()
            for capture_index, target_offset in enumerate((0.0, 0.1, 0.25, 0.5)):
                remaining = handoff_started + target_offset - time.monotonic()
                if remaining > 0:
                    time.sleep(remaining)
                if process.poll() is not None:
                    raise RuntimeError(
                        f"MoerEditor exited during handoff capture: {process.returncode}"
                    )
                handoff_capture_path = outdir / f"02_main_handoff_{capture_index:02d}.bmp"
                handoff_capture = capture_print_window(main_hwnd, handoff_capture_path)
                handoff_capture["white_ratio"] = white_ratio(handoff_capture_path)
                handoff_capture["placeholder_gray_ratio"] = placeholder_gray_ratio(
                    handoff_capture_path
                )
                handoff_capture["phase"] = "covered_by_splash"
                handoff_capture["handoff_offset_seconds"] = time.monotonic() - handoff_started
                report["captures"].append(handoff_capture)

            handoff_deadline = time.monotonic() + args.handoff_timeout
            while time.monotonic() < handoff_deadline:
                windows = visible_windows(process.pid)
                if not find_window(windows, title=SPLASH_TITLE, class_name=SPLASH_CLASS):
                    record("splash_closed", windows)
                    reveal_capture_path = outdir / "02_main_reveal.bmp"
                    reveal_capture = capture_print_window(main_hwnd, reveal_capture_path)
                    reveal_capture["white_ratio"] = white_ratio(reveal_capture_path)
                    reveal_capture["placeholder_gray_ratio"] = placeholder_gray_ratio(
                        reveal_capture_path
                    )
                    reveal_capture["phase"] = "splash_reveal"
                    report["captures"].append(reveal_capture)
                    if reveal_capture["placeholder_gray_ratio"] > 0.60:
                        failures.append(
                            "The Splash revealed the main window while it still contained the "
                            "neutral-gray bootstrap frame"
                        )
                    break
                time.sleep(0.05)
            else:
                failures.append("Splash remained visible after the main window handoff")

            time.sleep(2.0)
            main_capture_path = outdir / "03_main_window.bmp"
            main_capture = capture_print_window(main_hwnd, main_capture_path)
            main_capture["white_ratio"] = white_ratio(main_capture_path)
            report["captures"].append(main_capture)

            closed_normally, exit_code = close_process(process, main_hwnd, args.close_timeout)
            report["closed_normally"] = closed_normally
            report["exit_code"] = exit_code
            if not closed_normally:
                failures.append("MoerEditor did not close normally")
            if exit_code != 0:
                failures.append(f"MoerEditor exit code was {exit_code}")
    except Exception as error:
        failures.append(str(error))
    finally:
        if process is not None and process.poll() is None:
            _, exit_code = close_process(process, main_hwnd or splash_hwnd, min(args.close_timeout, 15.0))
            report["exit_code"] = exit_code

    report["duration_seconds"] = time.monotonic() - started
    forbidden_matches = scan_forbidden(stdout_path, stderr_path)
    report["forbidden_log_matches"] = forbidden_matches
    if forbidden_matches:
        failures.append("Forbidden error/validation patterns were found in startup logs")

    stdout_text = (
        stdout_path.read_text(encoding="utf-8", errors="replace")
        if stdout_path.exists()
        else ""
    )
    first_present_log_count = stdout_text.count(FIRST_PRESENT_LOG)
    report["first_present_log_count"] = first_present_log_count
    if first_present_log_count != 1:
        failures.append(
            "Expected exactly one confirmed first-present log before handoff, "
            f"observed {first_present_log_count}"
        )
    content_ready_log_count = stdout_text.count(CONTENT_READY_LOG)
    report["content_ready_log_count"] = content_ready_log_count
    if content_ready_log_count != 1:
        failures.append(
            "Expected the normal startup handoff to arm exactly one scene-ready Present receipt, "
            f"observed {content_ready_log_count}"
        )

    for capture in report["captures"]:
        if capture["nonblack_ratio"] < 0.01:
            failures.append(f"Capture is effectively blank: {capture['path']}")
        # Handoff samples are taken while the opaque splash still covers the main
        # window. Raster exposure may legitimately start out almost white there;
        # only the reveal and stable captures are user-visible quality gates.
        if (
            capture.get("phase") != "covered_by_splash"
            and capture["white_ratio"] > 0.60
        ):
            failures.append(f"Capture is predominantly white: {capture['path']}")

    report["success"] = not failures
    report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report, indent=2))
    return 0 if report["success"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
