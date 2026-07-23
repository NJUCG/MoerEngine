from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path
from typing import Sequence


SUMMARY_RE = re.compile(
    r"\[ParallelRecord\] batch=(?P<batch>\d+).*?effective=(?P<effective>true|false) "
    r"outcome=(?P<outcome>\S+).*?work_units=(?P<work_units>\d+) "
    r"ordered_cb=(?P<ordered_cb>\d+) "
    r"waves=(?P<waves>\d+) islands=(?P<islands>\d+).*?"
    r"distinct_workers=(?P<workers>\d+) max_active=(?P<max_active>\d+)"
)
INJECTION_RE = re.compile(
    r"\[ParallelRecord\]\[Injection\].*?point=worker-throw "
    r"phase=after-first-command.*?batch=(?P<batch>\d+)"
)


class VulkanTestError(RuntimeError):
    pass


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise VulkanTestError(message)


def validate_log(mode: str, text: str) -> None:
    _require("[TESTCASE][FAIL]" not in text, f"{mode}: test emitted a FAIL marker")
    _require("VUID-" not in text, f"{mode}: Vulkan validation VUID was emitted")
    _require("Validation Error" not in text, f"{mode}: Vulkan validation error was emitted")
    expected_fault = "true" if mode == "fallback" else "false"
    expected_mode = "serial" if mode == "serial" else "parallel"
    expected_production_gate = "true" if mode in ("gated", "heavy") else "false"
    expected_production_heavy = "true" if mode == "heavy" else "false"
    _require(
        re.search(
            rf"\[TESTCASE\]\[PASS\].*mode={expected_mode}.*worker_fault={expected_fault}"
            rf".*production_gate={expected_production_gate}"
            rf".*production_heavy={expected_production_heavy}",
            text,
        )
        is not None,
        f"{mode}: missing matching PASS marker",
    )

    summaries = [match.groupdict() for match in SUMMARY_RE.finditer(text)]
    if mode == "serial":
        _require(not summaries, "serial: parallel backend summary was unexpectedly emitted")
        _require("[ParallelRecord][Injection]" not in text, "serial: fault injection ran")
        return
    if mode == "gated":
        _require(not summaries, "gated: cheap workload unexpectedly recorded in parallel")
        _require(
            "reason=insufficient-work" in text,
            "gated: production threshold did not report insufficient work",
        )
        _require("[ParallelRecord][Wave]" not in text, "gated: worker wave was dispatched")
        _require("[ParallelRecord][Injection]" not in text, "gated: fault injection ran")
        return

    _require(summaries, f"{mode}: no parallel backend summary was emitted")
    _require(
        any(
            item["outcome"] == "parallel"
            and int(item["work_units"]) > 0
            and int(item["ordered_cb"]) > 1
            and int(item["waves"]) >= 2
            and int(item["islands"]) >= 1
            and int(item["workers"]) >= 2
            and int(item["max_active"]) >= 2
            for item in summaries
        ),
        f"{mode}: no effective concurrent multi-CB batch with two waves and a serial island",
    )
    if mode == "heavy":
        _require(
            any(int(item["work_units"]) >= 128 for item in summaries),
            "heavy: production floor did not admit two threshold-sized jobs",
        )

    lines = text.splitlines()
    first_parallel_batch = next(
        item["batch"] for item in summaries if item["outcome"] == "parallel"
    )
    wave_positions = [
        index
        for index, line in enumerate(lines)
        if f"[ParallelRecord][Wave] batch={first_parallel_batch} " in line
    ]
    island_positions = [
        index
        for index, line in enumerate(lines)
        if f"[ParallelRecord][Island] batch={first_parallel_batch} " in line
    ]
    _require(
        any(
            any(wave < island for wave in wave_positions)
            and any(wave > island for wave in wave_positions)
            for island in island_positions
        ),
        f"{mode}: logs do not prove joined wave -> serial island -> later wave order",
    )

    injection_batches = [match.group("batch") for match in INJECTION_RE.finditer(text)]
    fallback_batches = [
        item["batch"]
        for item in summaries
        if item["outcome"] == "serial-fallback-worker-failure"
    ]
    if mode in ("parallel", "heavy"):
        _require(not injection_batches, "parallel: unexpected worker fault injection")
        _require(not fallback_batches, "parallel: unexpected worker fallback")
    else:
        _require(
            len(injection_batches) == 1,
            f"fallback: expected one post-command injection, got {len(injection_batches)}",
        )
        _require(
            len(fallback_batches) == 1,
            f"fallback: expected one fallback batch, got {len(fallback_batches)}",
        )
        fault_batch = fallback_batches[0]
        _require(
            injection_batches[0] == fault_batch,
            "fallback: injection and coordinator fallback belong to different batches",
        )
        _require(
            re.search(
                rf"\[ParallelRecord\]\[Wave\] batch={fault_batch} .*?worker_recorded=false",
                text,
            )
            is not None,
            "fallback: injected batch has no failed worker wave",
        )


def run_case(executable: Path, outdir: Path, mode: str, timeout: float) -> Path:
    arguments: list[str] = []
    if mode != "serial":
        arguments.append("--parallel")
    if mode == "fallback":
        arguments.append("--inject-worker-failure")
    if mode == "gated":
        arguments.append("--production-gate")
    if mode == "heavy":
        arguments.append("--production-heavy")

    completed = subprocess.run(
        [str(executable), *arguments],
        cwd=executable.parent,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=timeout,
        check=False,
    )
    text = completed.stdout + completed.stderr
    outdir.mkdir(parents=True, exist_ok=True)
    log_path = outdir / f"{mode}.log"
    log_path.write_text(text, encoding="utf-8")
    _require(completed.returncode == 0, f"{mode}: executable returned {completed.returncode}")
    validate_log(mode, text)
    return log_path


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run and gate the Vulkan parallel-record serial/parallel/fallback matrix."
    )
    parser.add_argument("--executable", required=True, type=Path)
    parser.add_argument("--outdir", required=True, type=Path)
    parser.add_argument("--timeout", type=float, default=180.0)
    args = parser.parse_args(argv)
    if args.timeout <= 0.0:
        parser.error("--timeout must be positive")
    return args


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    executable = args.executable.resolve()
    if not executable.is_file():
        print(f"parallel Vulkan test: executable not found: {executable}", file=sys.stderr)
        return 2
    try:
        logs = [
            run_case(executable, args.outdir, mode, args.timeout)
            for mode in ("serial", "parallel", "fallback", "gated", "heavy")
        ]
    except (OSError, subprocess.TimeoutExpired, VulkanTestError) as error:
        print(f"parallel Vulkan test: FAIL: {error}", file=sys.stderr)
        return 1
    print("parallel Vulkan test: PASS: " + ", ".join(str(path) for path in logs))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
