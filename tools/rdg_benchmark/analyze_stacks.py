"""Aggregate sample_process folded stacks. Counts are wall-time samples, not CPU cycles."""
import argparse
import collections
import json
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("folded", type=Path)
args = parser.parse_args()
inclusive, leaves, phases = (collections.Counter() for _ in range(3))
total = 0
for line in args.folded.read_text(encoding="utf-8").splitlines():
    stack_text, count_text = line.rsplit(" ", 1)
    count = int(count_text)
    stack = stack_text.split(";")
    total += count
    leaves[stack[-1]] += count
    inclusive.update({name: count for name in set(stack)})
    for name in reversed(stack):
        if "RenderGraphCompiler::" in name or name.endswith("RenderGraphLowering::Lower"):
            phases[name] += count
            break

def rows(counter):
    return [{"symbol": symbol, "samples": count, "percent": round(count * 100 / total, 3)}
            for symbol, count in counter.most_common()]

result = {"samples": total, "leaf": rows(leaves), "inclusive": rows(inclusive), "compiler_lower": rows(phases)}
args.folded.with_suffix(".summary.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
print(json.dumps({"samples": total, "leaf_top": rows(leaves)[:12], "compiler_lower": rows(phases)[:15]}, indent=2))
