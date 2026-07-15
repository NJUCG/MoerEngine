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


SW_MINIMIZE = 6
SW_RESTORE = 9
SW_MAXIMIZE = 3
WM_CLOSE = 0x0010
WM_KEYDOWN = 0x0100
WM_KEYUP = 0x0101
MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004
PW_RENDERFULLCONTENT = 2
DIB_RGB_COLORS = 0

DEFAULT_FORBIDDEN_PATTERNS = (
    r"\[error\]",
    r"assertion failed",
    r"\bVUID-",
    r"device lost",
    r"vkQueueSubmit2 FAILED",
    r"tracked buffer",
    r"remaining allocation",
    r"access violation",
)

if os.name == "nt":
    user32 = ctypes.windll.user32
    gdi32 = ctypes.windll.gdi32
else:
    user32 = None
    gdi32 = None


class RECT(ctypes.Structure):
    _fields_ = [
        ("left", ctypes.c_long),
        ("top", ctypes.c_long),
        ("right", ctypes.c_long),
        ("bottom", ctypes.c_long),
    ]


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [
        ("biSize", wintypes.DWORD),
        ("biWidth", ctypes.c_long),
        ("biHeight", ctypes.c_long),
        ("biPlanes", wintypes.WORD),
        ("biBitCount", wintypes.WORD),
        ("biCompression", wintypes.DWORD),
        ("biSizeImage", wintypes.DWORD),
        ("biXPelsPerMeter", ctypes.c_long),
        ("biYPelsPerMeter", ctypes.c_long),
        ("biClrUsed", wintypes.DWORD),
        ("biClrImportant", wintypes.DWORD),
    ]


class BITMAPINFO(ctypes.Structure):
    _fields_ = [("bmiHeader", BITMAPINFOHEADER), ("bmiColors", wintypes.DWORD * 3)]


def window_rect(hwnd: int) -> RECT:
    rect = RECT()
    if not user32.GetWindowRect(hwnd, ctypes.byref(rect)):
        raise RuntimeError(f"GetWindowRect failed for hwnd={hwnd}")
    return rect


def enumerate_windows(pid: int, min_width: int = 120, min_height: int = 100) -> list[int]:
    handles: list[int] = []
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    def callback(hwnd: int, _: int) -> bool:
        window_pid = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(window_pid))
        if window_pid.value == pid and user32.IsWindowVisible(hwnd):
            rect = window_rect(hwnd)
            width = rect.right - rect.left
            height = rect.bottom - rect.top
            if width >= min_width and height >= min_height:
                handles.append(hwnd)
        return True

    callback_ref = callback_type(callback)
    user32.EnumWindows(callback_ref, 0)
    handles.sort(
        key=lambda hwnd: (window_rect(hwnd).right - window_rect(hwnd).left)
        * (window_rect(hwnd).bottom - window_rect(hwnd).top),
        reverse=True,
    )
    return handles


def find_main_window(process: subprocess.Popen[bytes], timeout: float) -> int:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            return 0
        handles = enumerate_windows(process.pid, 320, 240)
        if handles:
            return handles[0]
        time.sleep(0.25)
    return 0


def capture_print_window(hwnd: int, path: Path) -> dict[str, object]:
    rect = window_rect(hwnd)
    width = rect.right - rect.left
    height = rect.bottom - rect.top
    if width <= 0 or height <= 0:
        raise RuntimeError(f"Invalid capture extent {width}x{height} for hwnd={hwnd}")

    screen_dc = user32.GetDC(0)
    memory_dc = gdi32.CreateCompatibleDC(screen_dc)
    bitmap = gdi32.CreateCompatibleBitmap(screen_dc, width, height)
    old_bitmap = gdi32.SelectObject(memory_dc, bitmap)
    try:
        if not user32.PrintWindow(hwnd, memory_dc, PW_RENDERFULLCONTENT):
            raise RuntimeError(f"PrintWindow failed for hwnd={hwnd}")

        info = BITMAPINFO()
        info.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
        info.bmiHeader.biWidth = width
        info.bmiHeader.biHeight = -height
        info.bmiHeader.biPlanes = 1
        info.bmiHeader.biBitCount = 32
        byte_count = width * height * 4
        pixels = ctypes.create_string_buffer(byte_count)
        lines = gdi32.GetDIBits(
            memory_dc,
            bitmap,
            0,
            height,
            pixels,
            ctypes.byref(info),
            DIB_RGB_COLORS,
        )
        if lines != height:
            raise RuntimeError(f"GetDIBits returned {lines}/{height} lines")

        header = bytes(info.bmiHeader)
        with path.open("wb") as output:
            output.write(
                struct.pack(
                    "<2sIHHI",
                    b"BM",
                    14 + len(header) + byte_count,
                    0,
                    0,
                    14 + len(header),
                )
            )
            output.write(header)
            output.write(pixels.raw)

        stride = max(1, (width * height) // 200_000)
        sampled = memoryview(pixels.raw).cast("B")
        nonblack = 0
        channel_sum = 0
        count = 0
        for pixel_index in range(0, width * height, stride):
            offset = pixel_index * 4
            blue, green, red = sampled[offset : offset + 3]
            nonblack += int(red > 5 or green > 5 or blue > 5)
            channel_sum += red + green + blue
            count += 1

        return {
            "path": str(path),
            "hwnd": int(hwnd),
            "width": width,
            "height": height,
            "nonblack_ratio": nonblack / max(count, 1),
            "mean_rgb": channel_sum / max(count * 3, 1),
        }
    finally:
        gdi32.SelectObject(memory_dc, old_bitmap)
        gdi32.DeleteObject(bitmap)
        gdi32.DeleteDC(memory_dc)
        user32.ReleaseDC(0, screen_dc)


def capture(report: dict[str, object], hwnd: int, path: Path) -> None:
    try:
        report["captures"].append(capture_print_window(hwnd, path))
    except Exception as error:
        report["capture_errors"].append(str(error))


def wait_process_alive(process: subprocess.Popen[bytes], seconds: float) -> None:
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        exit_code = process.poll()
        if exit_code is not None:
            raise RuntimeError(f"MoerEditor exited early with code {exit_code}")
        time.sleep(min(0.25, max(0.0, deadline - time.monotonic())))


def wait_for_log(
    process: subprocess.Popen[bytes], path: Path, pattern: str, timeout: float
) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            if pattern in path.read_text(encoding="utf-8", errors="replace"):
                return True
        except OSError:
            pass
        if process.poll() is not None:
            return False
        time.sleep(0.5)
    return False


def drag_window_tab(
    hwnd: int, start_x_ratio: float, start_y_offset: int, destination_x: int, destination_y: int
) -> None:
    rect = window_rect(hwnd)
    start_x = rect.left + int((rect.right - rect.left) * start_x_ratio)
    start_y = rect.top + start_y_offset
    user32.ShowWindow(hwnd, SW_RESTORE)
    user32.BringWindowToTop(hwnd)
    user32.SetForegroundWindow(hwnd)
    user32.SetCursorPos(start_x, start_y)
    user32.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
    time.sleep(0.1)
    user32.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)
    time.sleep(0.4)

    user32.SetCursorPos(start_x, start_y)
    user32.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
    time.sleep(0.25)
    for step in range(1, 41):
        x = start_x + (destination_x - start_x) * step // 40
        y = start_y + (destination_y - start_y) * step // 40
        user32.SetCursorPos(x, y)
        time.sleep(0.04)
    user32.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)


def scan_logs(
    paths: list[Path],
    required: list[str],
    forbidden: list[str],
    allowed_forbidden: list[str],
    required_counts: list[tuple[str, int]],
) -> tuple[list[str], list[dict[str, object]], list[dict[str, object]]]:
    texts = {
        str(path): path.read_text(encoding="utf-8", errors="replace")
        if path.exists()
        else ""
        for path in paths
    }
    combined = "\n".join(texts.values())
    missing_required = [pattern for pattern in required if pattern not in combined]

    log_count_results = []
    for pattern, expected in required_counts:
        regex = re.compile(pattern, re.MULTILINE)
        actual = sum(len(regex.findall(text)) for text in texts.values())
        log_count_results.append(
            {
                "pattern": pattern,
                "expected": expected,
                "actual": actual,
                "matches": actual == expected,
            }
        )

    forbidden_matches: list[dict[str, object]] = []
    compiled = [(pattern, re.compile(pattern, re.IGNORECASE)) for pattern in forbidden]
    compiled_allowed = [re.compile(pattern, re.IGNORECASE) for pattern in allowed_forbidden]
    for path, text in texts.items():
        for line_number, line in enumerate(text.splitlines(), start=1):
            if any(regex.search(line) for regex in compiled_allowed):
                continue
            for pattern, regex in compiled:
                if regex.search(line):
                    forbidden_matches.append(
                        {
                            "path": path,
                            "line": line_number,
                            "pattern": pattern,
                            "text": line,
                        }
                    )
    return missing_required, forbidden_matches, log_count_results


def close_process(
    process: subprocess.Popen[bytes], hwnd: int, timeout: float
) -> tuple[bool, int | None]:
    if process.poll() is not None:
        return False, process.returncode
    if hwnd:
        user32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
    try:
        process.wait(timeout=timeout)
        return True, process.returncode
    except subprocess.TimeoutExpired:
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=10)
        return False, process.returncode


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Launch MoerEditor, stress its windows, capture frames, and validate logs."
    )
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--workdir", type=Path, required=True)
    parser.add_argument("--outdir", type=Path, required=True)
    parser.add_argument("--config", type=Path)
    parser.add_argument("--editor-arg", action="append", default=[])
    parser.add_argument("--startup-timeout", type=float, default=60.0)
    parser.add_argument("--startup-settle-seconds", type=float, default=15.0)
    parser.add_argument("--ready-log-pattern")
    parser.add_argument("--ready-timeout", type=float, default=180.0)
    parser.add_argument("--ready-settle-seconds", type=float, default=12.0)
    parser.add_argument("--soak-seconds", type=float, default=0.0)
    parser.add_argument("--close-timeout", type=float, default=45.0)
    parser.add_argument("--skip-window-stress", action="store_true")
    parser.add_argument("--no-maximize", action="store_true")
    parser.add_argument("--detach-configs", action="store_true")
    parser.add_argument("--min-nonblack-ratio", type=float, default=0.01)
    parser.add_argument("--require-log-pattern", action="append", default=[])
    parser.add_argument(
        "--require-log-count",
        action="append",
        nargs=2,
        metavar=("PATTERN", "COUNT"),
        default=[],
        help="Require a regex to occur exactly COUNT times across stdout and stderr.",
    )
    parser.add_argument("--forbid-log-pattern", action="append", default=[])
    parser.add_argument(
        "--allow-forbidden-log-pattern",
        action="append",
        default=[],
        help=(
            "Allow a line matching this regex even if it also matches a forbidden "
            "pattern; all other lines retain the normal forbidden scan."
        ),
    )
    parser.add_argument("--no-default-forbidden-patterns", action="store_true")
    args = parser.parse_args()

    required_counts: list[tuple[str, int]] = []
    for pattern, count_text in args.require_log_count:
        try:
            expected = int(count_text)
        except ValueError:
            parser.error(f"--require-log-count COUNT must be an integer: {count_text!r}")
        if expected < 0:
            parser.error("--require-log-count COUNT cannot be negative")
        required_counts.append((pattern, expected))
    args.require_log_count = required_counts

    regex_options = (
        [("--require-log-count", pattern) for pattern, _ in required_counts]
        + [("--forbid-log-pattern", pattern) for pattern in args.forbid_log_pattern]
        + [
            ("--allow-forbidden-log-pattern", pattern)
            for pattern in args.allow_forbidden_log_pattern
        ]
    )
    for option, pattern in regex_options:
        try:
            re.compile(pattern)
        except re.error as error:
            parser.error(f"{option} contains an invalid regex {pattern!r}: {error}")
    return args


def main() -> int:
    args = parse_args()
    if os.name != "nt":
        print("runtime_verify.py currently supports Windows only.", file=sys.stderr)
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
        print(f"Config override does not exist: {config}", file=sys.stderr)
        return 2

    outdir.mkdir(parents=True, exist_ok=True)
    stdout_path = outdir / "stdout.log"
    stderr_path = outdir / "stderr.log"
    report_path = outdir / "report.json"
    command = [str(exe)]
    if config is not None:
        command.extend(["--config", str(config)])
    command.extend(args.editor_arg)

    report: dict[str, object] = {
        "command": command,
        "workdir": str(workdir),
        "config": str(config) if config else None,
        "captures": [],
        "capture_errors": [],
        "required_log_patterns": args.require_log_pattern,
        "required_log_counts": [
            {"pattern": pattern, "expected": expected}
            for pattern, expected in args.require_log_count
        ],
        "forbidden_log_patterns": (
            [] if args.no_default_forbidden_patterns else list(DEFAULT_FORBIDDEN_PATTERNS)
        )
        + args.forbid_log_pattern,
        "allowed_forbidden_log_patterns": args.allow_forbidden_log_pattern,
        "closed_normally": False,
        "success": False,
    }

    started = time.monotonic()
    process: subprocess.Popen[bytes] | None = None
    main_hwnd = 0
    automation_error: str | None = None
    try:
        with stdout_path.open("wb") as stdout, stderr_path.open("wb") as stderr:
            process = subprocess.Popen(command, cwd=workdir, stdout=stdout, stderr=stderr)
            report["pid"] = process.pid
            main_hwnd = find_main_window(process, args.startup_timeout)
            if not main_hwnd:
                raise RuntimeError("MoerEditor main window was not found")
            report["main_hwnd"] = int(main_hwnd)

            if args.detach_configs:
                user32.ShowWindow(main_hwnd, SW_RESTORE)
                user32.SetWindowPos(main_hwnd, 0, 0, 0, 1600, 900, 0x0002 | 0x0004)
            elif not args.no_maximize:
                user32.ShowWindow(main_hwnd, SW_MAXIMIZE)

            if args.ready_log_pattern:
                if not wait_for_log(
                    process,
                    stdout_path,
                    args.ready_log_pattern,
                    args.ready_timeout,
                ):
                    raise RuntimeError(
                        f"Ready log pattern was not observed: {args.ready_log_pattern}"
                    )
                wait_process_alive(process, args.ready_settle_seconds)
            else:
                wait_process_alive(process, args.startup_settle_seconds)

            if args.detach_configs:
                for start_x_ratio in (0.765, 0.75, 0.78):
                    drag_window_tab(main_hwnd, start_x_ratio, 70, 1900, 300)
                    wait_process_alive(process, 3.0)
                    if len(enumerate_windows(process.pid)) > 1:
                        break
                wait_process_alive(process, 2.0)

            capture(report, main_hwnd, outdir / "01_maximized.bmp")
            viewport_windows = [
                hwnd for hwnd in enumerate_windows(process.pid) if hwnd != main_hwnd
            ]
            report["platform_viewport_count"] = len(viewport_windows)
            for index, viewport_hwnd in enumerate(viewport_windows):
                capture(report, viewport_hwnd, outdir / f"01_viewport_{index}.bmp")

            if args.detach_configs and viewport_windows:
                user32.SetWindowPos(viewport_windows[0], 0, 0, 0, 700, 650, 0x0002 | 0x0004)
                wait_process_alive(process, 4.0)
                capture(report, viewport_windows[0], outdir / "01_viewport_resized.bmp")

            if not args.skip_window_stress:
                user32.ShowWindow(main_hwnd, SW_RESTORE)
                user32.SetWindowPos(main_hwnd, 0, 0, 0, 1600, 900, 0x0002 | 0x0004)
                wait_process_alive(process, 3.0)
                capture(report, main_hwnd, outdir / "02_resized.bmp")

                user32.ShowWindow(main_hwnd, SW_MINIMIZE)
                wait_process_alive(process, 2.0)
                user32.ShowWindow(main_hwnd, SW_MAXIMIZE)
                wait_process_alive(process, 4.0)
                capture(report, main_hwnd, outdir / "03_restored.bmp")

                user32.PostMessageW(main_hwnd, WM_KEYDOWN, 0x44, 0)
                wait_process_alive(process, 2.0)
                user32.PostMessageW(main_hwnd, WM_KEYUP, 0x44, 0)
                wait_process_alive(process, 2.0)
                capture(report, main_hwnd, outdir / "04_after_key.bmp")

            if args.soak_seconds > 0.0:
                wait_process_alive(process, args.soak_seconds)
                capture(report, main_hwnd, outdir / "05_after_soak.bmp")

            closed_normally, exit_code = close_process(
                process, main_hwnd, args.close_timeout
            )
            report["closed_normally"] = closed_normally
            report["exit_code"] = exit_code
    except Exception as error:
        automation_error = str(error)
    finally:
        if process is not None:
            if process.poll() is None:
                _, exit_code = close_process(
                    process, main_hwnd, min(args.close_timeout, 15.0)
                )
                report["exit_code"] = exit_code
            else:
                report["exit_code"] = process.returncode

    report["duration_seconds"] = time.monotonic() - started
    if automation_error is not None:
        report["automation_error"] = automation_error

    forbidden = report["forbidden_log_patterns"]
    missing_required, forbidden_matches, log_count_results = scan_logs(
        [stdout_path, stderr_path],
        args.require_log_pattern,
        forbidden,
        args.allow_forbidden_log_pattern,
        args.require_log_count,
    )
    report["missing_required_log_patterns"] = missing_required
    report["forbidden_log_matches"] = forbidden_matches
    report["required_log_count_results"] = log_count_results

    failure_reasons: list[str] = []
    if automation_error is not None:
        failure_reasons.append(automation_error)
    if not report.get("closed_normally"):
        failure_reasons.append("MoerEditor did not close normally")
    if report.get("exit_code") != 0:
        failure_reasons.append(f"MoerEditor exit code was {report.get('exit_code')}")
    if report["capture_errors"]:
        failure_reasons.append("One or more window captures failed")
    if not report["captures"]:
        failure_reasons.append("No window captures were produced")
    for item in report["captures"]:
        if item["nonblack_ratio"] < args.min_nonblack_ratio:
            failure_reasons.append(
                f"Capture {item['path']} has nonblack_ratio={item['nonblack_ratio']:.6f}"
            )
    if missing_required:
        failure_reasons.append("Required log patterns were not observed")
    if any(not result["matches"] for result in log_count_results):
        failure_reasons.append("Required log counts did not match")
    if forbidden_matches:
        failure_reasons.append("Forbidden log patterns were observed")

    report["failure_reasons"] = failure_reasons
    report["success"] = not failure_reasons
    report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report, indent=2))
    return 0 if report["success"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
