"""Summarize paired allocator samples; no frame-rate claims are inferred."""
from __future__ import annotations

import argparse
import csv
import json
import random
import statistics
from collections import defaultdict
from pathlib import Path


def summarize(path: Path) -> dict:
    groups = defaultdict(lambda: defaultdict(dict))
    capacity = {}
    raw = path.read_bytes()
    # Windows PowerShell 5 writes UTF-16 for >, while PowerShell 7 writes UTF-8.
    encoding = "utf-16" if raw.startswith((b"\xff\xfe", b"\xfe\xff")) else "utf-8-sig"
    for row in csv.reader(raw.decode(encoding).splitlines()):
        if row[0] == "capacity":
            capacity[row[1]] = {"legacy": int(row[2]), "current": int(row[3])}
        elif row[0] == "timing":
            _, impl, threads, size, count, sample, ns = row
            groups[(int(threads), int(size), int(count))][impl][int(sample)] = float(ns)
    result = []
    rng = random.Random(42)
    for (threads, size, count), impls in sorted(groups.items()):
        old, new = impls["legacy"], impls["current"]
        if old.keys() != new.keys() or len(old) != 40:
            raise ValueError("expected 40 paired samples per case")
        ratios = [new[k] / old[k] for k in old]
        bootstrap = sorted(statistics.median(rng.choices(ratios, k=len(ratios))) for _ in range(2000))
        result.append({
            "threads": threads, "bytes": size, "operations": count,
            "legacy_median_ns": statistics.median(old.values()),
            "current_median_ns": statistics.median(new.values()),
            "paired_median_ratio": statistics.median(ratios),
            "ratio_bootstrap_95ci": [bootstrap[50], bootstrap[1949]],
            "legacy_p95_ns": sorted(old.values())[37],
            "current_p95_ns": sorted(new.values())[37],
        })
    if len(result) != 12:
        raise ValueError("expected all 12 workloads")
    return {"capacity": capacity, "timing": result}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("raw", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    result = summarize(args.raw)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print("threads bytes legacy_ns current_ns paired_ratio 95%CI")
    for row in result["timing"]:
        lo, hi = row["ratio_bootstrap_95ci"]
        print(f'{row["threads"]:7} {row["bytes"]:5} {row["legacy_median_ns"]:9.2f} '
              f'{row["current_median_ns"]:10.2f} {row["paired_median_ratio"]:12.2f} [{lo:.2f}, {hi:.2f}]')
