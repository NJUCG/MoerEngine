from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import time
import tomllib
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path


@dataclass(frozen=True)
class Scenario:
    name: str
    renderer: str
    render_thread: bool
    rhi_thread: bool
    rhi_bypass: bool
    max_frame_lag: int
    window_stress: bool = False

    @property
    def effective_rhi_thread(self) -> bool:
        return self.rhi_thread and not self.rhi_bypass


SCENARIOS = {
    scenario.name: scenario
    for scenario in (
        Scenario("raster_sync", "Raster", False, False, True, 0),
        Scenario("raster_rhi_bypass", "Raster", False, True, True, 0),
        Scenario("raster_rhi_gt", "Raster", False, True, False, 0, True),
        Scenario("raster_rt0_rhi", "Raster", True, True, False, 0),
        Scenario("raster_rt1_rhi", "Raster", True, True, False, 1, True),
        Scenario("raster_rt1_rhi_off", "Raster", True, False, False, 1),
        Scenario("ray_rhi_gt", "Raytracing", False, True, False, 0),
        Scenario("ray_rt0_rhi", "Raytracing", True, True, False, 0),
        Scenario("ray_rt1_rhi", "Raytracing", True, True, False, 1, True),
    )
}

SCENARIO_SETS = {
    "smoke": ("raster_sync", "raster_rhi_gt", "ray_rt1_rhi"),
    "full": tuple(SCENARIOS),
    "soak": ("raster_rt1_rhi", "ray_rt1_rhi"),
}

SECTION_PATTERN = re.compile(r"^\s*\[([^]]+)]\s*(?:#.*)?$")
ASSIGNMENT_PATTERN = re.compile(r"^(?P<indent>\s*)(?P<key>[A-Za-z0-9_-]+)\s*=")


def toml_literal(value: object) -> str:
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, int):
        return str(value)
    if isinstance(value, str):
        return json.dumps(value)
    raise TypeError(f"Unsupported TOML scalar: {value!r}")


def generate_config(base_text: str, scenario: Scenario) -> str:
    replacements = {
        ("engine.threading", "render_thread"): scenario.render_thread,
        ("engine.threading", "rhi_thread"): scenario.rhi_thread,
        ("engine.threading", "rhi_bypass"): scenario.rhi_bypass,
        ("engine.threading", "max_frame_lag"): scenario.max_frame_lag,
        ("engine.threading", "profile_logging"): True,
        ("engine.render", "default_render_method"): scenario.renderer,
    }
    replaced = {target: 0 for target in replacements}
    current_section = ""
    output: list[str] = []

    for line in base_text.splitlines(keepends=True):
        line_without_newline = line.rstrip("\r\n")
        newline = line[len(line_without_newline) :]
        section_match = SECTION_PATTERN.match(line_without_newline)
        if section_match:
            current_section = section_match.group(1).strip()
            output.append(line)
            continue

        assignment_match = ASSIGNMENT_PATTERN.match(line_without_newline)
        if assignment_match:
            target = (current_section, assignment_match.group("key"))
            if target in replacements:
                output.append(
                    f"{assignment_match.group('indent')}{target[1]} = "
                    f"{toml_literal(replacements[target])}{newline}"
                )
                replaced[target] += 1
                continue
        output.append(line)

    invalid = [target for target, count in replaced.items() if count != 1]
    if invalid:
        details = ", ".join(f"{section}.{key}={replaced[(section, key)]}" for section, key in invalid)
        raise ValueError(f"Base config does not contain each required key exactly once: {details}")

    generated = "".join(output)
    parsed = tomllib.loads(generated)
    threading = parsed["engine"]["threading"]
    render = parsed["engine"]["render"]
    actual = {
        ("engine.threading", "render_thread"): threading["render_thread"],
        ("engine.threading", "rhi_thread"): threading["rhi_thread"],
        ("engine.threading", "rhi_bypass"): threading["rhi_bypass"],
        ("engine.threading", "max_frame_lag"): threading["max_frame_lag"],
        ("engine.threading", "profile_logging"): threading["profile_logging"],
        ("engine.render", "default_render_method"): render["default_render_method"],
    }
    if actual != replacements:
        raise ValueError(f"Generated config validation failed: expected={replacements}, actual={actual}")
    return generated


def bool_text(value: bool) -> str:
    return "true" if value else "false"


def required_patterns(scenario: Scenario) -> list[str]:
    execution_thread = "Render" if scenario.render_thread else "Game"
    patterns = [
        "[Config] Using command-line config override:",
        (
            f"[Threading] render_thread={bool_text(scenario.render_thread)}, "
            f"rhi_thread={bool_text(scenario.rhi_thread)}, "
            f"rhi_bypass={bool_text(scenario.rhi_bypass)}, "
            f"max_frame_lag={scenario.max_frame_lag}"
        ),
        (
            "[Threading] Vulkan graphics queue RHI mode: "
            + ("threaded" if scenario.effective_rhi_thread else "synchronous")
        ),
        (
            f"[Threading] {scenario.renderer} frames execute on "
            f"{execution_thread} thread id ="
        ),
        "[ThreadingProfile][RHI]",
    ]
    if scenario.effective_rhi_thread:
        patterns.append("[Threading] RHIThread id =")
    if scenario.render_thread and scenario.max_frame_lag == 1:
        patterns.append("[Threading] GT/RT overlap active")
    if scenario.render_thread:
        patterns.append("[ThreadingProfile][RT]")
    return patterns


def build_verifier_command(
    verifier: Path,
    exe: Path,
    workdir: Path,
    run_dir: Path,
    config_path: Path,
    scenario: Scenario,
    args: argparse.Namespace,
) -> list[str]:
    command = [
        sys.executable,
        str(verifier),
        "--exe",
        str(exe),
        "--workdir",
        str(workdir),
        "--outdir",
        str(run_dir),
        "--config",
        str(config_path),
        "--startup-timeout",
        str(args.startup_timeout),
        "--ready-log-pattern",
        args.ready_log_pattern,
        "--ready-timeout",
        str(args.ready_timeout),
        "--ready-settle-seconds",
        str(args.ready_settle_seconds),
        "--close-timeout",
        str(args.close_timeout),
        "--min-nonblack-ratio",
        str(args.min_nonblack_ratio),
    ]
    for pattern in required_patterns(scenario):
        command.extend(["--require-log-pattern", pattern])
    if not scenario.effective_rhi_thread:
        command.extend(["--forbid-log-pattern", r"\[Threading\] RHIThread id ="])
    if args.skip_window_stress or not scenario.window_stress:
        command.append("--skip-window-stress")
    if args.detach_configs:
        command.append("--detach-configs")
    if args.soak_seconds > 0.0:
        command.extend(["--soak-seconds", str(args.soak_seconds)])
    return command


def parse_profile_value(value: str) -> object:
    try:
        return int(value)
    except ValueError:
        try:
            return float(value)
        except ValueError:
            return value


def parse_profile_windows(path: Path) -> dict[str, list[dict[str, object]]]:
    profiles: dict[str, list[dict[str, object]]] = {"RHI": [], "RT": []}
    if not path.is_file():
        return profiles

    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        for profile_type in profiles:
            marker = f"[ThreadingProfile][{profile_type}]"
            marker_index = line.find(marker)
            if marker_index < 0:
                continue
            fields = {
                key: parse_profile_value(value)
                for key, value in re.findall(
                    r"([A-Za-z_]+)=([^\s]+)", line[marker_index + len(marker) :]
                )
            }
            profiles[profile_type].append(fields)
            break
    return profiles


def aggregate_profile_windows(
    windows: list[dict[str, object]], profile_type: str, tail_count: int
) -> dict[str, object]:
    selected = windows[-tail_count:] if tail_count > 0 else windows
    if not selected:
        return {}

    sample_key = "samples" if profile_type == "RHI" else "frames"
    sample_count = sum(int(window.get(sample_key, 0)) for window in selected)
    window_ms = sum(float(window.get("window_ms", 0.0)) for window in selected)

    def weighted_average(field: str) -> float:
        if sample_count == 0:
            return 0.0
        return sum(
            float(window.get(field, 0.0)) * int(window.get(sample_key, 0))
            for window in selected
        ) / sample_count

    def weighted_average_by(field: str, count_field: str) -> float:
        count = sum(int(window.get(count_field, 0)) for window in selected)
        if count == 0:
            return 0.0
        return sum(
            float(window.get(field, 0.0)) * int(window.get(count_field, 0))
            for window in selected
        ) / count

    aggregate: dict[str, object] = {
        "window_count": len(selected),
        "sample_count": sample_count,
        "window_ms": window_ms,
        "samples_per_second": sample_count * 1000.0 / window_ms if window_ms > 0.0 else 0.0,
        "windows": selected,
    }
    if profile_type == "RHI":
        aggregate.update(
            {
                "mode": selected[-1].get("mode"),
                "caller_avg_ms": weighted_average("caller_avg_ms"),
                "caller_max_ms": max(float(window.get("caller_max_ms", 0.0)) for window in selected),
                "queue_wait_avg_ms": weighted_average("queue_wait_avg_ms"),
                "queue_wait_max_ms": max(
                    float(window.get("queue_wait_max_ms", 0.0)) for window in selected
                ),
                "work_avg_ms": weighted_average("work_avg_ms"),
                "work_max_ms": max(float(window.get("work_max_ms", 0.0)) for window in selected),
                "execute_caller_avg_ms": weighted_average_by(
                    "execute_caller_avg_ms", "execute"
                ),
                "execute_wait_avg_ms": weighted_average_by("execute_wait_avg_ms", "execute"),
                "execute_work_avg_ms": weighted_average_by("execute_work_avg_ms", "execute"),
                "present_caller_avg_ms": weighted_average_by(
                    "present_caller_avg_ms", "present"
                ),
                "present_wait_avg_ms": weighted_average_by("present_wait_avg_ms", "present"),
                "present_work_avg_ms": weighted_average_by("present_work_avg_ms", "present"),
                "max_enqueue_depth": max(
                    int(window.get("max_enqueue_depth", 0)) for window in selected
                ),
                "max_gpu_pending": max(int(window.get("gpu_pending", 0)) for window in selected),
            }
        )
    else:
        aggregate.update(
            {
                "prepare_avg_ms": weighted_average("prepare_avg_ms"),
                "prepare_max_ms": max(float(window.get("prepare_max_ms", 0.0)) for window in selected),
                "queue_wait_avg_ms": weighted_average("queue_wait_avg_ms"),
                "queue_wait_max_ms": max(
                    float(window.get("queue_wait_max_ms", 0.0)) for window in selected
                ),
                "render_avg_ms": weighted_average("render_avg_ms"),
                "render_max_ms": max(float(window.get("render_max_ms", 0.0)) for window in selected),
                "gt_wait_avg_ms": weighted_average("gt_wait_avg_ms"),
                "gt_wait_max_ms": max(float(window.get("gt_wait_max_ms", 0.0)) for window in selected),
                "max_pending": max(int(window.get("max_pending", 0)) for window in selected),
            }
        )
    return aggregate


def profile_number(run: dict[str, object], profile: str, field: str) -> float:
    data = run.get(profile, {})
    return float(data.get(field, 0.0)) if isinstance(data, dict) else 0.0


def write_markdown_summary(path: Path, summary: dict[str, object]) -> None:
    lines = [
        "# RT/RHI Threading Validation",
        "",
        f"- Started: `{summary['started_at']}`",
        f"- Duration: `{summary['duration_seconds']:.1f}s`",
        f"- Result: `{'PASS' if summary['success'] else 'FAIL'}`",
        "",
        "| Scenario | Iteration | Result | RHI caller | RHI wait | RHI work | Depth | RT queue | RT render | GT wait |",
        "|---|---:|---|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for run in summary["runs"]:
        lines.append(
            f"| {run['scenario']} | {run['iteration']} | "
            f"{'PASS' if run['success'] else 'FAIL'} | "
            f"{profile_number(run, 'rhi_profile', 'caller_avg_ms'):.3f} ms | "
            f"{profile_number(run, 'rhi_profile', 'queue_wait_avg_ms'):.3f} ms | "
            f"{profile_number(run, 'rhi_profile', 'work_avg_ms'):.3f} ms | "
            f"{profile_number(run, 'rhi_profile', 'max_enqueue_depth'):.0f} | "
            f"{profile_number(run, 'rt_profile', 'queue_wait_avg_ms'):.3f} ms | "
            f"{profile_number(run, 'rt_profile', 'render_avg_ms'):.3f} ms | "
            f"{profile_number(run, 'rt_profile', 'gt_wait_avg_ms'):.3f} ms |"
        )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def parse_args(repo_root: Path) -> argparse.Namespace:
    default_exe = repo_root / "target" / "bin" / "Debug" / "MoerEditor.exe"
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    parser = argparse.ArgumentParser(
        description="Run repeatable MoerEngine RT/RHI runtime validation matrices."
    )
    parser.add_argument("--exe", type=Path, default=default_exe)
    parser.add_argument("--workdir", type=Path)
    parser.add_argument("--base-config", type=Path, default=repo_root / "MoerEngine.toml")
    parser.add_argument(
        "--outdir",
        type=Path,
        default=repo_root / "target" / "validation" / "rt_rhi" / timestamp,
    )
    parser.add_argument("--set", choices=tuple(SCENARIO_SETS), default="smoke")
    parser.add_argument("--scenario", action="append", choices=tuple(SCENARIOS))
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--soak-seconds", type=float)
    parser.add_argument("--startup-timeout", type=float, default=90.0)
    parser.add_argument("--ready-timeout", type=float, default=180.0)
    parser.add_argument("--ready-settle-seconds", type=float, default=5.0)
    parser.add_argument("--close-timeout", type=float, default=45.0)
    parser.add_argument("--ready-log-pattern", default="Copied ImGui frame includes")
    parser.add_argument("--min-nonblack-ratio", type=float, default=0.01)
    parser.add_argument("--profile-tail-windows", type=int, default=5)
    parser.add_argument("--skip-window-stress", action="store_true")
    parser.add_argument("--detach-configs", action="store_true")
    parser.add_argument("--continue-on-failure", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--list", action="store_true")
    args = parser.parse_args()
    if args.repeat < 1:
        parser.error("--repeat must be at least 1")
    if args.soak_seconds is None:
        args.soak_seconds = 300.0 if args.set == "soak" else 0.0
    if args.soak_seconds < 0.0:
        parser.error("--soak-seconds cannot be negative")
    if args.profile_tail_windows < 1:
        parser.error("--profile-tail-windows must be at least 1")
    return args


def main() -> int:
    repo_root = Path(__file__).resolve().parents[2]
    verifier = Path(__file__).resolve().with_name("runtime_verify.py")
    args = parse_args(repo_root)
    if args.list:
        for name, scenario in SCENARIOS.items():
            print(
                f"{name}: renderer={scenario.renderer}, rt={scenario.render_thread}, "
                f"rhi={scenario.rhi_thread}, bypass={scenario.rhi_bypass}, "
                f"lag={scenario.max_frame_lag}, stress={scenario.window_stress}"
            )
        return 0

    exe = args.exe.resolve()
    workdir = args.workdir.resolve() if args.workdir else exe.parent
    base_config = args.base_config.resolve()
    outdir = args.outdir.resolve()
    if not verifier.is_file():
        print(f"Verifier does not exist: {verifier}", file=sys.stderr)
        return 2
    if not args.dry_run and not exe.is_file():
        print(f"MoerEditor executable does not exist: {exe}", file=sys.stderr)
        return 2
    if not base_config.is_file():
        print(f"Base config does not exist: {base_config}", file=sys.stderr)
        return 2

    selected_names = args.scenario or list(SCENARIO_SETS[args.set])
    base_text = base_config.read_text(encoding="utf-8")

    planned_run_dirs = []
    for iteration in range(1, args.repeat + 1):
        for scenario_name in selected_names:
            run_name = (
                scenario_name if args.repeat == 1 else f"{scenario_name}_{iteration:03d}"
            )
            planned_run_dirs.append(outdir / run_name)
    nonempty_run_dirs = [
        path for path in planned_run_dirs if path.is_dir() and any(path.iterdir())
    ]
    if nonempty_run_dirs:
        print(
            "Refusing to reuse non-empty run directories:\n  "
            + "\n  ".join(str(path) for path in nonempty_run_dirs),
            file=sys.stderr,
        )
        return 2

    outdir.mkdir(parents=True, exist_ok=True)

    started_at = datetime.now().astimezone().isoformat()
    started = time.monotonic()
    runs: list[dict[str, object]] = []
    stop = False
    for iteration in range(1, args.repeat + 1):
        for scenario_name in selected_names:
            scenario = SCENARIOS[scenario_name]
            run_name = scenario.name if args.repeat == 1 else f"{scenario.name}_{iteration:03d}"
            run_dir = outdir / run_name
            run_dir.mkdir(parents=True, exist_ok=True)
            config_path = run_dir / "MoerEngine.toml"
            config_path.write_text(generate_config(base_text, scenario), encoding="utf-8")
            command = build_verifier_command(
                verifier,
                exe,
                workdir,
                run_dir,
                config_path,
                scenario,
                args,
            )
            print(f"[{iteration}/{args.repeat}] {scenario.name}")
            print("  " + subprocess.list2cmdline(command))

            if args.dry_run:
                runs.append(
                    {
                        "scenario": scenario.name,
                        "iteration": iteration,
                        "success": True,
                        "dry_run": True,
                        "command": command,
                    }
                )
                continue

            completed = subprocess.run(command, capture_output=True, text=True)
            report_path = run_dir / "report.json"
            report = (
                json.loads(report_path.read_text(encoding="utf-8"))
                if report_path.is_file()
                else {}
            )
            captures = report.get("captures", [])
            profile_windows = parse_profile_windows(run_dir / "stdout.log")
            rhi_profile = aggregate_profile_windows(
                profile_windows["RHI"], "RHI", args.profile_tail_windows
            )
            rt_profile = aggregate_profile_windows(
                profile_windows["RT"], "RT", args.profile_tail_windows
            )
            min_nonblack = min(
                (capture.get("nonblack_ratio", 0.0) for capture in captures),
                default=0.0,
            )
            success = completed.returncode == 0 and bool(report.get("success"))
            run = {
                "scenario": scenario.name,
                "iteration": iteration,
                "success": success,
                "verifier_exit_code": completed.returncode,
                "exit_code": report.get("exit_code"),
                "closed_normally": report.get("closed_normally", False),
                "capture_count": len(captures),
                "min_nonblack_ratio": min_nonblack,
                "duration_seconds": report.get("duration_seconds", 0.0),
                "failure_reasons": report.get("failure_reasons", []),
                "rhi_profile": rhi_profile,
                "rt_profile": rt_profile,
                "report": str(report_path),
            }
            runs.append(run)
            print(
                f"  {'PASS' if success else 'FAIL'}: exit={run['exit_code']}, "
                f"captures={run['capture_count']}, min_nonblack={min_nonblack:.4f}"
            )
            if not success:
                if completed.stdout:
                    print(completed.stdout, file=sys.stderr)
                if completed.stderr:
                    print(completed.stderr, file=sys.stderr)
                if not args.continue_on_failure:
                    stop = True
                    break
        if stop:
            break

    summary = {
        "started_at": started_at,
        "duration_seconds": time.monotonic() - started,
        "matrix_set": args.set,
        "selected_scenarios": selected_names,
        "repeat": args.repeat,
        "soak_seconds": args.soak_seconds,
        "profile_tail_windows": args.profile_tail_windows,
        "dry_run": args.dry_run,
        "success": all(run["success"] for run in runs)
        and len(runs) == len(selected_names) * args.repeat,
        "runs": runs,
    }
    (outdir / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    write_markdown_summary(outdir / "summary.md", summary)
    print(f"Summary: {outdir / 'summary.md'}")
    return 0 if summary["success"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
