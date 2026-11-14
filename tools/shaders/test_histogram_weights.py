"""Validate histogram interpolation and compile both exposure histogram producers.

Run with Python and spirv-val on PATH. Numeric checks compile the same pure
weight helper used by Raster and Raytracing; no engine build is required.
"""

from pathlib import Path
import re
import shutil
import subprocess


REPO = Path(__file__).resolve().parents[2]
DXC = REPO / "3rdparty/dxc_2026_02_20/bin/x64/dxc.exe"
OUTPUT = REPO / "build/shader-regressions/histogram"
INCLUDES = [
    REPO / "shaders",
    REPO / "source/runtime/render/shaderheaders",
    REPO / "3rdparty/dxc_2026_02_20/inc",
    REPO / "3rdparty/dxc_2026_02_20/inc/hlsl",
]


def compile_shader(source: Path, name: str, stage: str, vulkan: bool) -> Path:
    target = OUTPUT / (name + (".spv" if vulkan else ".dxil"))
    command = [str(DXC), str(source), "-T", stage + ("_6_7" if vulkan else "_6_8"),
               "-E", "main", "-O3", "-all-resources-bound"]
    for directory in INCLUDES:
        command += ["-I", str(directory)]
    if vulkan:
        command += ["-spirv", "-fspv-target-env=vulkan1.3", "-fvk-use-dx-layout",
                    "-fvk-auto-shift-bindings", "-fspv-preserve-interface", "-DVULKAN=1"]
    else:
        command += ["-DDXIL=1", "-Qstrip_debug", "-Qstrip_reflect"]
    command += ["-Fo", str(target)]
    compiled = subprocess.run(command, capture_output=True, text=True, errors="replace")
    diagnostics = compiled.stdout + compiled.stderr
    (OUTPUT / (name + ".log")).write_text(diagnostics, encoding="utf-8")
    if compiled.returncode:
        raise RuntimeError(diagnostics)
    if vulkan:
        validator = shutil.which("spirv-val")
        if validator is None:
            raise RuntimeError("spirv-val must be on PATH for Vulkan shader validation")
        subprocess.run([validator, "--target-env", "vulkan1.3", "--scalar-block-layout", str(target)],
                       check=True, capture_output=True, text=True)
    return target


def main() -> None:
    OUTPUT.mkdir(parents=True, exist_ok=True)
    probe = REPO / "tools/shaders/probes/HistogramBinWeightsRegression.hlsl"
    dxil = compile_shader(probe, "weights-probe-dxil", "ps", False)
    assembly = subprocess.check_output([str(DXC), "-dumpbin", str(dxil)], text=True)
    (OUTPUT / "weights-probe.dxil.txt").write_text(assembly, encoding="utf-8")
    constants = re.findall(r"call void @dx\.op\.storeOutput\.f32\([^\n]*, float ([^ )]+)\)", assembly)
    if [float(value) for value in constants] != [0.0, 64.0, 0.0, 64.0]:
        raise AssertionError(f"Histogram weight, mass or first-moment regression: {constants}")
    compile_shader(probe, "weights-probe-vulkan", "ps", True)

    producers = {
        "raster": "shaders/pipelines/postprocess/color/TonemappingHistogram.hlsl",
        "raytracing": "shaders/pipelines/raytracing/postprocess/Histogram.hlsl",
    }
    for name, source in producers.items():
        compile_shader(REPO / source, name + "-histogram-dxil", "cs", False)
        compile_shader(REPO / source, name + "-histogram-vulkan", "cs", True)
    print("Histogram regression passed: integer/fractional weights, first moments and highest-bin mass.")
    print("Compiled the probe and both producers for DXIL/Vulkan; all three SPIR-V modules validated.")


if __name__ == "__main__":
    main()
