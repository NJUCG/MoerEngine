"""Summarize current-vs-VMA paired CPU samples; these are not GPU/FPS results."""
from __future__ import annotations

import argparse
import csv
import json
import random
import statistics
from collections import defaultdict
from pathlib import Path


def summarize(path: Path) -> dict:
    raw = path.read_bytes()
    encoding = "utf-16" if raw.startswith((b"\xff\xfe", b"\xfe\xff")) else "utf-8-sig"
    groups = defaultdict(lambda: defaultdict(dict))
    for row in csv.DictReader(raw.decode(encoding).splitlines()):
        if row["kind"] != "timing":
            raise ValueError("unexpected benchmark record")
        key = (row["mode"], int(row["threads"]), int(row["bytes"]), int(row["operations"]))
        samples = groups[key][row["implementation"]]
        sample = int(row["sample"])
        if sample in samples:
            raise ValueError("duplicate sample")
        samples[sample] = {k: float(row[k]) for k in
                           ("ns_per_op", "snapshot_ns_per_op", "retire_ns_per_op", "ranges")}
    rng = random.Random(42)
    results = []
    for (mode, threads, size, count), implementations in sorted(groups.items()):
        current, vma = implementations["current"], implementations["vma"]
        if current.keys() != vma.keys() or set(current) != set(range(40)):
            raise ValueError("expected 40 paired samples per workload")
        ratios = [vma[k]["ns_per_op"] / current[k]["ns_per_op"] for k in current]
        bootstrap = sorted(statistics.median(rng.choices(ratios, k=40)) for _ in range(2000))
        result = {"mode": mode, "threads": threads, "bytes": size, "operations": count,
                  "paired_median_ratio": statistics.median(ratios),
                  "ratio_bootstrap_95ci": [bootstrap[50], bootstrap[1949]]}
        for name, samples in implementations.items():
            result[name] = {"median_" + k: statistics.median(sample[k] for sample in samples.values())
                            for k in ("ns_per_op", "snapshot_ns_per_op", "retire_ns_per_op", "ranges")}
            result[name]["p95_ns_per_op"] = sorted(sample["ns_per_op"] for sample in samples.values())[37]
        results.append(result)
    expected = {(mode, threads, size, 4096 if size == 256 else 16384)
                for mode in ("shared", "independent") for threads in (1, 2, 4, 8)
                for size in (0, 32, 256) if mode == "shared" or threads != 1}
    if set(groups) != expected:
        raise ValueError("expected all 21 workloads")
    return {"timing": results}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("raw", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    result = summarize(args.raw)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print("mode        threads bytes current_ns vma_ns ratio 95%CI  retire_current/vma_ns ranges_current/vma")
    for row in result["timing"]:
        current, vma = row["current"], row["vma"]
        lo, hi = row["ratio_bootstrap_95ci"]
        print(f'{row["mode"]:11} {row["threads"]:7} {row["bytes"]:5} '
              f'{current["median_ns_per_op"]:10.2f} {vma["median_ns_per_op"]:7.2f} '
              f'{row["paired_median_ratio"]:5.2f} [{lo:.2f}, {hi:.2f}] '
              f'{current["median_retire_ns_per_op"]:.2f}/{vma["median_retire_ns_per_op"]:.2f} '
              f'{current["median_ranges"]:g}/{vma["median_ranges"]:g}')
