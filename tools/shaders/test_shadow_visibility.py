"""Check the PCSS search/filter regression using the production shader macros.

Run from any directory with Python, DXC and spirv-val available. Outputs go to
build/shader-regressions; this does not build or launch the engine.
"""

from pathlib import Path
import re
import shutil
import subprocess


REPO = Path(__file__).resolve().parents[2]
DXC = REPO / "3rdparty/dxc_2026_02_20/bin/x64/dxc.exe"
OUTPUT = REPO / "build/shader-regressions/pcss"
INCLUDES = [
    REPO / "shaders",
    REPO / "source/runtime/render/shaderheaders",
    REPO / "3rdparty/dxc_2026_02_20/inc",
    REPO / "3rdparty/dxc_2026_02_20/inc/hlsl",
]


def compile_shader(source: Path, name: str, vulkan: bool) -> Path:
    target = OUTPUT / (name + (".spv" if vulkan else ".dxil"))
    command = [str(DXC), str(source), "-T", "ps_6_7" if vulkan else "ps_6_8",
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
    probe = REPO / "tools/shaders/probes/PcssFullBlockerRegression.hlsl"
    dxil = compile_shader(probe, "disk-probe-dxil", False)
    assembly = subprocess.check_output([str(DXC), "-dumpbin", str(dxil)], text=True)
    (OUTPUT / "disk-probe.dxil.txt").write_text(assembly, encoding="utf-8")
    constants = re.findall(r"call void @dx\.op\.storeOutput\.f32\([^\n]*, float ([^ )]+)\)", assembly)
    if constants != ["9.375000e-01"]:
        raise AssertionError(f"Expected the constant PCSS visibility 15/16, got output operands {constants}")
    compile_shader(probe, "disk-probe-vulkan", True)

    shadow_mask = REPO / "shaders/pipelines/raster/deferred/lighting/shadows/ShadowMask.frag.hlsl"
    compile_shader(shadow_mask, "shadow-mask-dxil", False)
    compile_shader(shadow_mask, "shadow-mask-vulkan", True)
    print("PCSS regression passed: full blocker search still filters to visibility 15/16.")
    print("Compiled the probe and ShadowMask for DXIL/Vulkan; both SPIR-V modules validated.")


if __name__ == "__main__":
    main()
