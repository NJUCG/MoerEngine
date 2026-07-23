from __future__ import annotations

import argparse
import json
import math
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence


PROFILE_MARKER = "[ParallelRecordProfile]"
PROFILE_MODES = ("serial", "parallel", "mixed")
INTEGER_FIELDS = ("samples", "requested", "planned", "effective", "worker_fallbacks")
WEIGHTED_FIELDS = (
    "record_p50_ms",
    "record_p95_ms",
    "record_p99_ms",
    "execute_cpu_p50_ms",
    "execute_cpu_p95_ms",
    "execute_cpu_p99_ms",
    "reorder_avg_ms",
    "preprocess_avg_ms",
    "worker_join_avg_ms",
    "submit_cpu_avg_ms",
    "jobs_avg",
    "work_units_avg",
    "ordered_cb_avg",
)
MAX_FIELDS = ("record_max_ms", "execute_cpu_max_ms", "max_active")
REQUIRED_FIELDS = INTEGER_FIELDS + WEIGHTED_FIELDS + MAX_FIELDS
KEY_VALUE_RE = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)\s*=\s*([^\s,;\]]+)")


class ProfileParseError(ValueError):
    pass


@dataclass(frozen=True)
class ProfileWindow:
    source: str
    line: int
    mode: str
    samples: int
    requested: int
    planned: int
    effective: int
    worker_fallbacks: int
    record_p50_ms: float
    record_p95_ms: float
    record_p99_ms: float
    record_max_ms: float
    execute_cpu_p50_ms: float
    execute_cpu_p95_ms: float
    execute_cpu_p99_ms: float
    execute_cpu_max_ms: float
    reorder_avg_ms: float
    preprocess_avg_ms: float
    worker_join_avg_ms: float
    submit_cpu_avg_ms: float
    jobs_avg: float
    work_units_avg: float
    ordered_cb_avg: float
    max_active: float


def _parse_nonnegative_int(value: str, field: str, location: str) -> int:
    try:
        parsed = int(value, 10)
    except ValueError as error:
        raise ProfileParseError(
            f"{location}: {field} must be an integer, got {value!r}"
        ) from error
    if parsed < 0:
        raise ProfileParseError(f"{location}: {field} must be non-negative")
    return parsed


def _parse_nonnegative_float(value: str, field: str, location: str) -> float:
    try:
        parsed = float(value)
    except ValueError as error:
        raise ProfileParseError(
            f"{location}: {field} must be numeric, got {value!r}"
        ) from error
    if not math.isfinite(parsed) or parsed < 0.0:
        raise ProfileParseError(f"{location}: {field} must be finite and non-negative")
    return parsed


def parse_profile_line(text: str, source: str, line_number: int) -> ProfileWindow | None:
    marker_offset = text.find(PROFILE_MARKER)
    if marker_offset < 0:
        return None

    payload = text[marker_offset + len(PROFILE_MARKER) :]
    # Keep structured siblings such as [ParallelRecordProfile][Config] out of
    # the aggregation stream. Data windows always separate the marker and the
    # first key with whitespace.
    if payload and not payload[0].isspace():
        return None
    fields: dict[str, str] = {}
    for match in KEY_VALUE_RE.finditer(payload):
        key, value = match.groups()
        if key in fields:
            raise ProfileParseError(
                f"{source}:{line_number}: duplicate {key!r} field in profile window"
            )
        fields[key] = value

    missing = [field for field in ("mode",) + REQUIRED_FIELDS if field not in fields]
    if missing:
        raise ProfileParseError(
            f"{source}:{line_number}: profile window is missing fields: "
            + ", ".join(missing)
        )

    location = f"{source}:{line_number}"
    mode = fields["mode"]
    if mode not in PROFILE_MODES:
        raise ProfileParseError(
            f"{location}: mode must be one of {', '.join(PROFILE_MODES)}, got {mode!r}"
        )
    integers = {
        field: _parse_nonnegative_int(fields[field], field, location)
        for field in INTEGER_FIELDS
    }
    numbers = {
        field: _parse_nonnegative_float(fields[field], field, location)
        for field in WEIGHTED_FIELDS + MAX_FIELDS
    }

    if integers["samples"] == 0:
        raise ProfileParseError(f"{location}: samples must be greater than zero")
    if integers["requested"] > integers["samples"]:
        raise ProfileParseError(f"{location}: requested cannot exceed samples")
    if integers["planned"] > integers["requested"]:
        raise ProfileParseError(f"{location}: planned cannot exceed requested")
    if integers["effective"] > integers["planned"]:
        raise ProfileParseError(f"{location}: effective cannot exceed planned")
    if integers["worker_fallbacks"] > integers["planned"]:
        raise ProfileParseError(f"{location}: worker_fallbacks cannot exceed planned")
    if integers["effective"] + integers["worker_fallbacks"] != integers["planned"]:
        raise ProfileParseError(
            f"{location}: effective + worker_fallbacks must equal planned"
        )
    all_samples_parallel = (
        integers["requested"] == integers["samples"]
        and integers["planned"] == integers["samples"]
        and integers["effective"] == integers["samples"]
        and integers["worker_fallbacks"] == 0
    )
    if mode == "serial" and any(
        integers[field] != 0
        for field in ("requested", "planned", "effective", "worker_fallbacks")
    ):
        raise ProfileParseError(f"{location}: serial mode has non-serial counters")
    if mode == "parallel" and not all_samples_parallel:
        raise ProfileParseError(f"{location}: parallel mode does not cover every sample")
    if mode == "mixed" and (
        integers["requested"] != integers["samples"] or all_samples_parallel
    ):
        raise ProfileParseError(f"{location}: mixed mode counters are inconsistent")
    if numbers["record_p50_ms"] > numbers["record_p95_ms"]:
        raise ProfileParseError(f"{location}: record_p50_ms cannot exceed record_p95_ms")
    if numbers["record_p95_ms"] > numbers["record_p99_ms"]:
        raise ProfileParseError(f"{location}: record_p95_ms cannot exceed record_p99_ms")
    if numbers["record_p99_ms"] > numbers["record_max_ms"]:
        raise ProfileParseError(f"{location}: record_p99_ms cannot exceed record_max_ms")
    if numbers["execute_cpu_p50_ms"] > numbers["execute_cpu_p95_ms"]:
        raise ProfileParseError(
            f"{location}: execute_cpu_p50_ms cannot exceed execute_cpu_p95_ms"
        )
    if numbers["execute_cpu_p95_ms"] > numbers["execute_cpu_p99_ms"]:
        raise ProfileParseError(
            f"{location}: execute_cpu_p95_ms cannot exceed execute_cpu_p99_ms"
        )
    if numbers["execute_cpu_p99_ms"] > numbers["execute_cpu_max_ms"]:
        raise ProfileParseError(
            f"{location}: execute_cpu_p99_ms cannot exceed execute_cpu_max_ms"
        )

    return ProfileWindow(
        source=source,
        line=line_number,
        mode=mode,
        **integers,
        **numbers,
    )


def load_profile_windows(path: Path, skip_windows: int) -> tuple[list[ProfileWindow], int]:
    if skip_windows < 0:
        raise ValueError("skip_windows must be non-negative")
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError as error:
        raise ProfileParseError(f"cannot read {path}: {error}") from error

    windows: list[ProfileWindow] = []
    for line_number, line in enumerate(text.splitlines(), start=1):
        window = parse_profile_line(line, str(path), line_number)
        if window is not None:
            windows.append(window)

    if not windows:
        raise ProfileParseError(f"{path}: no {PROFILE_MARKER} windows found")

    retained = windows[skip_windows:]
    if not retained:
        raise ProfileParseError(
            f"{path}: skip-windows={skip_windows} removed all {len(windows)} profile windows"
        )
    return retained, min(skip_windows, len(windows))


def aggregate_group(paths: Sequence[Path], skip_windows: int) -> dict[str, object]:
    windows: list[ProfileWindow] = []
    skipped = 0
    per_log: list[dict[str, object]] = []
    for path in paths:
        retained, skipped_count = load_profile_windows(path, skip_windows)
        windows.extend(retained)
        skipped += skipped_count
        per_log.append(
            {
                "path": str(path),
                "windows": len(retained),
                "skipped_windows": skipped_count,
                "samples": sum(window.samples for window in retained),
            }
        )

    samples = sum(window.samples for window in windows)
    requested = sum(window.requested for window in windows)
    planned = sum(window.planned for window in windows)
    effective = sum(window.effective for window in windows)
    worker_fallbacks = sum(window.worker_fallbacks for window in windows)
    result: dict[str, object] = {
        "logs": len(paths),
        "windows": len(windows),
        "skipped_windows": skipped,
        "samples": samples,
        "requested": requested,
        "planned": planned,
        "effective": effective,
        "worker_fallbacks": worker_fallbacks,
        "plan_rate": planned / requested if requested else None,
        "worker_success_rate": effective / planned if planned else None,
        "effective_rate": effective / requested if requested else None,
        "mode_counts": {
            mode: sum(window.mode == mode for window in windows)
            for mode in PROFILE_MODES
            if any(window.mode == mode for window in windows)
        },
        "per_log": per_log,
    }
    for field in WEIGHTED_FIELDS:
        result[field] = sum(getattr(window, field) * window.samples for window in windows) / samples
    for field in MAX_FIELDS:
        result[field] = max(getattr(window, field) for window in windows)
    return result


def validate_group_purity(group: dict[str, object], expected: str) -> None:
    samples = int(group["samples"])
    requested = int(group["requested"])
    planned = int(group["planned"])
    effective = int(group["effective"])
    worker_fallbacks = int(group["worker_fallbacks"])
    mode_counts = group["mode_counts"]
    assert isinstance(mode_counts, dict)

    if expected == "serial":
        if (
            requested != 0
            or planned != 0
            or effective != 0
            or worker_fallbacks != 0
            or set(mode_counts) != {"serial"}
        ):
            raise ProfileParseError(
                "serial logs are not a pure serial baseline: expected requested=planned="
                "effective=worker_fallbacks=0 and mode=serial, got "
                f"requested={requested}, planned={planned}, effective={effective}, "
                f"worker_fallbacks={worker_fallbacks}, mode_counts={mode_counts}"
            )
        return

    if expected == "parallel":
        invalid_modes = set(mode_counts) - {"parallel", "mixed"}
        if requested != samples or invalid_modes:
            raise ProfileParseError(
                "parallel logs are not a pure feature-on run: expected every sample "
                f"requested and modes parallel/mixed, got samples={samples}, "
                f"requested={requested}, mode_counts={mode_counts}"
            )
        return

    raise ValueError(f"unsupported expected group {expected!r}")


def _comparison(serial: float, parallel: float) -> dict[str, float | None]:
    delta = serial - parallel
    return {
        "serial": serial,
        "parallel": parallel,
        "delta_ms": delta,
        "improvement_ratio": delta / serial if serial > 0.0 else None,
    }


def make_report(
    serial: dict[str, object],
    parallel: dict[str, object],
    *,
    serial_logs: Sequence[Path],
    parallel_logs: Sequence[Path],
    skip_windows: int,
) -> dict[str, object]:
    min_worker_success_rate = 0.95
    min_p50_improvement_ratio = 0.15
    min_p50_improvement_ms = 0.2
    max_tail_regression_ratio = 0.03
    max_tail_regression_ms = 0.2

    planned = int(parallel["planned"])
    worker_success_rate = parallel["worker_success_rate"]
    worker_success_applicable = planned > 0
    worker_success_pass = (
        not worker_success_applicable
        or (
            worker_success_rate is not None
            and float(worker_success_rate) >= min_worker_success_rate
        )
    )

    # Record-only values remain useful diagnostics, but release gating uses the
    # end-to-end ExecuteNow CPU wall time so multi-CB submit overhead cannot be
    # hidden behind faster worker recording.
    comparisons: dict[str, object] = {
        field: _comparison(float(serial[field]), float(parallel[field]))
        for field in ("record_p50_ms", "record_p95_ms", "record_p99_ms")
    }
    serial_p50 = float(serial["execute_cpu_p50_ms"])
    parallel_p50 = float(parallel["execute_cpu_p50_ms"])
    p50 = _comparison(serial_p50, parallel_p50)
    comparisons["execute_cpu_p50_ms"] = p50
    p50_ratio = p50["improvement_ratio"]
    checks: list[dict[str, object]] = [
        {
            "name": "parallel_worker_success_rate",
            "pass": worker_success_pass,
            "applicable": worker_success_applicable,
            "actual": worker_success_rate,
            "requirement": f">= {min_worker_success_rate:.0%} when planned > 0",
        },
    ]

    if planned == 0:
        p50_regression = parallel_p50 - serial_p50
        p50_tolerance = max(
            serial_p50 * max_tail_regression_ratio, max_tail_regression_ms
        )
        p50_pass = p50_regression <= p50_tolerance + 1e-12
        p50.update(
            {
                "regression_ms": p50_regression,
                "allowed_regression_ms": p50_tolerance,
                "pass": p50_pass,
            }
        )
        checks.append(
            {
                "name": "execute_cpu_p50_gated_regression",
                "pass": p50_pass,
                "actual_ms": p50_regression,
                "allowed_ms": p50_tolerance,
                "requirement": (
                    "parallel - serial <= "
                    f"max({max_tail_regression_ratio:.0%} of serial, "
                    f"{max_tail_regression_ms:.3f} ms)"
                ),
            }
        )
    else:
        p50_pass = (
            p50_ratio is not None
            and p50_ratio >= min_p50_improvement_ratio
            and float(p50["delta_ms"]) >= min_p50_improvement_ms
        )
        checks.append(
            {
                "name": "execute_cpu_p50_improvement",
                "pass": p50_pass,
                "actual_ratio": p50_ratio,
                "actual_ms": p50["delta_ms"],
                "requirement": (
                    f">= {min_p50_improvement_ratio:.0%} and "
                    f">= {min_p50_improvement_ms:.3f} ms"
                ),
            }
        )

    for field in ("execute_cpu_p95_ms", "execute_cpu_p99_ms"):
        serial_value = float(serial[field])
        parallel_value = float(parallel[field])
        comparison = _comparison(serial_value, parallel_value)
        regression = parallel_value - serial_value
        tolerance = max(serial_value * max_tail_regression_ratio, max_tail_regression_ms)
        passed = regression <= tolerance + 1e-12
        comparison.update(
            {
                "regression_ms": regression,
                "allowed_regression_ms": tolerance,
                "pass": passed,
            }
        )
        comparisons[field] = comparison
        checks.append(
            {
                "name": f"{field}_regression",
                "pass": passed,
                "actual_ms": regression,
                "allowed_ms": tolerance,
                "requirement": (
                    "parallel - serial <= "
                    f"max({max_tail_regression_ratio:.0%} of serial, "
                    f"{max_tail_regression_ms:.3f} ms)"
                ),
            }
        )

    all_checks_pass = all(bool(check["pass"]) for check in checks)
    verdict = ("GATED" if planned == 0 else "GO") if all_checks_pass else "EXPERIMENTAL"
    return {
        "schema_version": 2,
        "verdict": verdict,
        "inputs": {
            "serial_logs": [str(path) for path in serial_logs],
            "parallel_logs": [str(path) for path in parallel_logs],
            "skip_windows_per_log": skip_windows,
        },
        "thresholds": {
            "min_worker_success_rate": min_worker_success_rate,
            "min_p50_improvement_ratio": min_p50_improvement_ratio,
            "min_p50_improvement_ms": min_p50_improvement_ms,
            "max_tail_regression_ratio": max_tail_regression_ratio,
            "max_tail_regression_ms": max_tail_regression_ms,
        },
        "serial": serial,
        "parallel": parallel,
        "comparisons": comparisons,
        "checks": checks,
    }


def _format_number(value: object, precision: int = 3) -> str:
    if value is None:
        return "n/a"
    return f"{float(value):.{precision}f}"


def _format_percent(value: object) -> str:
    if value is None:
        return "n/a"
    return f"{float(value):.2%}"


def render_markdown(report: dict[str, object]) -> str:
    serial = report["serial"]
    parallel = report["parallel"]
    assert isinstance(serial, dict)
    assert isinstance(parallel, dict)
    lines = [
        "# Parallel Command Recording A/B Report",
        "",
        f"**Verdict: {report['verdict']}**",
        "",
        "Percentiles below are sample-weighted means of the emitted aggregation-window percentiles; "
        "max fields are maxima across retained windows. Release gates use end-to-end ExecuteNow CPU time.",
        "",
        "| Group | Logs | Windows | Samples | Requested | Planned | Effective | Plan rate | Worker success |",
        "|---|---:|---:|---:|---:|---:|---:|---:|---:|",
        (
            f"| Serial | {serial['logs']} | {serial['windows']} | {serial['samples']} | "
            f"{serial['requested']} | {serial['planned']} | {serial['effective']} | "
            f"{_format_percent(serial['plan_rate'])} | "
            f"{_format_percent(serial['worker_success_rate'])} |"
        ),
        (
            f"| Parallel | {parallel['logs']} | {parallel['windows']} | {parallel['samples']} | "
            f"{parallel['requested']} | {parallel['planned']} | {parallel['effective']} | "
            f"{_format_percent(parallel['plan_rate'])} | "
            f"{_format_percent(parallel['worker_success_rate'])} |"
        ),
        "",
        "| Metric | Serial | Parallel |",
        "|---|---:|---:|",
    ]
    for field in WEIGHTED_FIELDS + MAX_FIELDS:
        lines.append(
            f"| `{field}` | {_format_number(serial[field])} | {_format_number(parallel[field])} |"
        )

    lines.extend(["", "## Gates", "", "| Check | Result | Actual | Requirement |", "|---|---|---|---|"])
    checks = report["checks"]
    assert isinstance(checks, list)
    for check in checks:
        assert isinstance(check, dict)
        if check["name"] == "parallel_worker_success_rate":
            actual_value = check["actual"]
            actual = "n/a" if actual_value is None else f"{float(actual_value):.2%}"
        elif check["name"].endswith("_improvement"):
            ratio = check["actual_ratio"]
            ratio_text = "n/a" if ratio is None else f"{float(ratio):.2%}"
            actual = f"{ratio_text}, {_format_number(check['actual_ms'])} ms"
        else:
            actual = f"{_format_number(check['actual_ms'])} ms"
        applicable = bool(check.get("applicable", True))
        result = "N/A" if not applicable else ("PASS" if check["pass"] else "FAIL")
        lines.append(
            f"| `{check['name']}` | {result} | "
            f"{actual} | {check['requirement']} |"
        )
    return "\n".join(lines) + "\n"


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Compare [ParallelRecordProfile] aggregation windows from serial and parallel runs."
    )
    parser.add_argument(
        "--serial-log",
        action="append",
        required=True,
        type=Path,
        help="Serial stdout.log; repeat for independent runs.",
    )
    parser.add_argument(
        "--parallel-log",
        action="append",
        required=True,
        type=Path,
        help="Parallel stdout.log; repeat for independent runs.",
    )
    parser.add_argument(
        "--skip-windows",
        type=int,
        default=0,
        help="Discard this many warm-up windows from the start of each log.",
    )
    parser.add_argument(
        "--min-runs-per-side",
        type=int,
        default=2,
        help="Require this many distinct process logs on both sides (default: 2).",
    )
    parser.add_argument(
        "--outdir",
        required=True,
        type=Path,
        help="Directory receiving report.json and report.md.",
    )
    args = parser.parse_args(argv)
    if args.skip_windows < 0:
        parser.error("--skip-windows must be non-negative")
    if args.min_runs_per_side < 1:
        parser.error("--min-runs-per-side must be positive")
    return args


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        serial_run_count = len({path.resolve() for path in args.serial_log})
        parallel_run_count = len({path.resolve() for path in args.parallel_log})
        if serial_run_count < args.min_runs_per_side:
            raise ProfileParseError(
                "serial side has "
                f"{serial_run_count} distinct run(s); require at least "
                f"{args.min_runs_per_side}"
            )
        if parallel_run_count < args.min_runs_per_side:
            raise ProfileParseError(
                "parallel side has "
                f"{parallel_run_count} distinct run(s); require at least "
                f"{args.min_runs_per_side}"
            )
        serial = aggregate_group(args.serial_log, args.skip_windows)
        parallel = aggregate_group(args.parallel_log, args.skip_windows)
        validate_group_purity(serial, "serial")
        validate_group_purity(parallel, "parallel")
        report = make_report(
            serial,
            parallel,
            serial_logs=args.serial_log,
            parallel_logs=args.parallel_log,
            skip_windows=args.skip_windows,
        )
        args.outdir.mkdir(parents=True, exist_ok=True)
        json_path = args.outdir / "report.json"
        markdown_path = args.outdir / "report.md"
        json_path.write_text(
            json.dumps(report, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        markdown_path.write_text(render_markdown(report), encoding="utf-8")
    except (OSError, ProfileParseError, ValueError) as error:
        print(f"parallel_record_ab: error: {error}", file=sys.stderr)
        return 2

    print(f"{report['verdict']}: {json_path}")
    return 0 if report["verdict"] == "GO" else 1


if __name__ == "__main__":
    raise SystemExit(main())
