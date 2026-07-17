#!/usr/bin/env python3
"""Summarize MoerEngine [RaytracingProfile] sliding-window logs."""

from __future__ import annotations

import argparse
import csv
import json
import math
import re
import statistics
from datetime import datetime
from pathlib import Path


PROFILE_MARKER = re.compile(r"\[RaytracingProfile\] GPU\(ms\)\s+(.*)$")
LOG_TIMESTAMP = re.compile(r"^\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}(?:\.\d+)?)\]")
METADATA_KEYS = {
    "TLASPolicy",
    "RendererTLASBuilds",
    "RendererTLASSkips",
    "SceneTLASUpdates",
    "RTRevision",
    "CurrentTLASRevision",
    "PreviousTLASRevision",
    "ConfiguredLocalLightSampling",
    "EffectiveLocalLightSampling",
    "AdaptiveFallback",
    "LocalLightSampling",
    "LocalLights",
}
METADATA_STRING_KEYS = {
    "TLASPolicy",
    "ConfiguredLocalLightSampling",
    "EffectiveLocalLightSampling",
    "AdaptiveFallback",
    "LocalLightSampling",
}
LEGACY_METADATA_RENAMES = {"LocalLightSampling": "LegacyLocalLightSampling"}
COUNTER_KEYS = (
    "RendererTLASBuilds",
    "RendererTLASSkips",
    "SceneTLASUpdates",
)


def percentile(sorted_values: list[float], fraction: float) -> float:
    if not sorted_values:
        raise ValueError("percentile requires at least one value")
    rank = (len(sorted_values) - 1) * fraction
    lower = math.floor(rank)
    upper = math.ceil(rank)
    if lower == upper:
        return sorted_values[lower]
    weight = rank - lower
    return sorted_values[lower] * (1.0 - weight) + sorted_values[upper] * weight


def summarize_values(values: list[float]) -> dict[str, float | int]:
    ordered = sorted(values)
    mean = statistics.fmean(ordered)
    stddev = statistics.pstdev(ordered)
    return {
        "n": len(ordered),
        "mean": mean,
        "median": statistics.median(ordered),
        "p90": percentile(ordered, 0.90),
        "p95": percentile(ordered, 0.95),
        "p99": percentile(ordered, 0.99),
        "min": ordered[0],
        "max": ordered[-1],
        "stddev": stddev,
        "cv": stddev / mean if mean else 0.0,
    }


def parse_log(path: Path, discard_first: int, before: datetime | None) -> dict[str, object]:
    records: list[tuple[dict[str, float], dict[str, str | int]]] = []

    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if before:
            timestamp_match = LOG_TIMESTAMP.match(line)
            if timestamp_match and datetime.fromisoformat(timestamp_match.group(1)) >= before:
                continue
        match = PROFILE_MARKER.search(line)
        if not match:
            continue

        timing_window: dict[str, float] = {}
        metadata: dict[str, str | int] = {}
        for token in match.group(1).split():
            if "=" not in token:
                continue
            key, value = token.split("=", 1)
            if key in METADATA_KEYS:
                output_key = LEGACY_METADATA_RENAMES.get(key, key)
                if key in METADATA_STRING_KEYS:
                    metadata[output_key] = value
                else:
                    try:
                        metadata[output_key] = int(value)
                    except ValueError:
                        metadata[output_key] = value
                continue
            try:
                timing_window[key] = float(value)
            except ValueError:
                continue
        if timing_window:
            records.append((timing_window, metadata))

    used_records = records[discard_first:]
    used_windows = [timings for timings, _ in used_records]
    timing_names = sorted({name for window in used_windows for name in window})
    timings: dict[str, dict[str, float | int]] = {}
    for name in timing_names:
        values = [window[name] for window in used_windows if name in window]
        if values:
            timings[name] = summarize_values(values)

    process_final_metadata = records[-1][1] if records else {}
    measurement_start_metadata = used_records[0][1] if used_records else {}
    measurement_end_metadata = used_records[-1][1] if used_records else {}
    measurement_counter_delta: dict[str, int] = {}
    for key in COUNTER_KEYS:
        start_value = measurement_start_metadata.get(key)
        end_value = measurement_end_metadata.get(key)
        if isinstance(start_value, int) and isinstance(end_value, int):
            measurement_counter_delta[key] = end_value - start_value

    return {
        "source": str(path.resolve()),
        "discard_first": discard_first,
        "before": before.isoformat(sep=" ") if before else None,
        "window_count_total": len(records),
        "window_count_used": len(used_windows),
        "process_final_metadata": process_final_metadata,
        "measurement_metadata": {
            "start": measurement_start_metadata,
            "end": measurement_end_metadata,
            "counter_delta": measurement_counter_delta,
        },
        "timings_ms": timings,
    }


def write_csv(path: Path, runs: dict[str, dict[str, object]]) -> None:
    fields = ["run", "scope", "n", "mean", "median", "p90", "p95", "p99", "min", "max", "stddev", "cv"]
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for run_name, run in runs.items():
            timings = run["timings_ms"]
            assert isinstance(timings, dict)
            for scope, stats in timings.items():
                assert isinstance(stats, dict)
                writer.writerow({"run": run_name, "scope": scope, **stats})


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", nargs="+", type=Path)
    parser.add_argument("--discard-first", type=int, default=5)
    parser.add_argument("--before", type=datetime.fromisoformat)
    parser.add_argument("--json", type=Path, dest="json_path")
    parser.add_argument("--csv", type=Path, dest="csv_path")
    args = parser.parse_args()

    if args.discard_first < 0:
        parser.error("--discard-first must be non-negative")

    runs: dict[str, dict[str, object]] = {}
    for path in args.logs:
        base_name = path.parent.name if path.stem.lower() in {"stdout", "stderr"} else path.stem
        run_name = base_name
        suffix = 2
        while run_name in runs:
            run_name = f"{base_name}_{suffix}"
            suffix += 1
        runs[run_name] = parse_log(path, args.discard_first, args.before)
    output = {"profile_semantics": "13-frame sliding GPU timestamp windows", "runs": runs}
    serialized = json.dumps(output, indent=2, ensure_ascii=False)
    print(serialized)

    if args.json_path:
        args.json_path.write_text(serialized + "\n", encoding="utf-8")
    if args.csv_path:
        write_csv(args.csv_path, runs)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
