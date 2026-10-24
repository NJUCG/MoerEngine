"""Run the same GPU readback workload through each Vulkan descriptor backend."""

import argparse
import json
import os
from pathlib import Path
import re
import subprocess


def main():
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, default=root / "target/bin/Debug/TestVulkanDescriptorBackend.exe")
    parser.add_argument("--validation-layer-dir", type=Path)
    parser.add_argument("--output", type=Path, default=root / "build/descriptor-backends")
    parser.add_argument("--modes", nargs="+", choices=("auto", "heap", "buffer", "sets"), default=("auto", "heap", "buffer", "sets"))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    executable = args.exe.resolve()
    results = []
    for mode in args.modes:
        for push in ("0", "1"):
            for parallel in (False, True):
                env = os.environ.copy()
                env.update(MOER_VULKAN_DESCRIPTORS=mode, MOER_VULKAN_PUSH_DESCRIPTORS=push, DISABLE_VULKAN_OBS_CAPTURE="1")
                if args.validation_layer_dir:
                    env["VK_LAYER_PATH"] = str(args.validation_layer_dir.resolve())
                log = args.output / f"{mode}-push{push}-parallel{int(parallel)}.log"
                timed_out = False
                with log.open("w", encoding="utf-8") as output:
                    try:
                        process = subprocess.run([str(executable)] + (["--parallel"] if parallel else []),
                                                 cwd=executable.parent, env=env, stdout=output,
                                                 stderr=subprocess.STDOUT, timeout=120, check=False)
                        code = process.returncode
                    except subprocess.TimeoutExpired:
                        code = None
                        timed_out = True
                text = log.read_text(encoding="utf-8", errors="replace")
                errors = [line for line in text.splitlines() if re.search(
                    r"VUID-|Validation Error|Undefined-Value|WARNING-CoreValidation|\[(?:error|critical)\]", line)]
                passed = re.search(r"\[PASS\] descriptor backend=(\w+).*elapsed_ms=(\d+)", text)
                unavailable = "Requested Vulkan descriptor backend is unavailable or invalid" in text
                status = "SKIP" if unavailable and mode != "auto" else "PASS" if code == 0 and passed and not errors else "FAIL"
                result = dict(mode=mode, push=push == "1", parallel=parallel, status=status,
                              exit_code=code, timeout=timed_out, validation_errors=len(errors), log=str(log))
                if passed:
                    result.update(selected=passed[1], elapsed_ms=int(passed[2]))
                results.append(result)
                print(json.dumps(result, ensure_ascii=False), flush=True)
    (args.output / "results.json").write_text(json.dumps(results, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    return int(any(result["status"] == "FAIL" for result in results))


if __name__ == "__main__":
    raise SystemExit(main())
