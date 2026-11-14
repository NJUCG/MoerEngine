"""Compile constant checks of the production AO hemisphere sampling helper.

The probe covers six axis normals and two oblique normals, with samples at the
pole, horizon and intermediate elevations. Requires DXC and spirv-val.
"""

from pathlib import Path
import re
import shutil
import subprocess


REPO = Path(__file__).resolve().parents[2]
DXC = REPO / "3rdparty/dxc_2026_02_20/bin/x64/dxc.exe"
OUTPUT = REPO / "build/shader-regressions/ao-geometry"


def compile_probe(vulkan: bool) -> Path:
    target = OUTPUT / ("hemisphere.spv" if vulkan else "hemisphere.dxil")
    command = [str(DXC), str(REPO / "tools/shaders/probes/AoGeometryRegression.hlsl"),
               "-T", "ps_6_7" if vulkan else "ps_6_8", "-E", "main", "-O3", "-all-resources-bound"]
    for directory in ["shaders", "source/runtime/render/shaderheaders",
                      "3rdparty/dxc_2026_02_20/inc", "3rdparty/dxc_2026_02_20/inc/hlsl"]:
        command += ["-I", str(REPO / directory)]
    if vulkan:
        command += ["-spirv", "-fspv-target-env=vulkan1.3", "-fvk-use-dx-layout",
                    "-fvk-auto-shift-bindings", "-fspv-preserve-interface", "-DVULKAN=1"]
    else:
        command += ["-DDXIL=1", "-Qstrip_debug", "-Qstrip_reflect"]
    command += ["-Fo", str(target)]
    compiled = subprocess.run(command, capture_output=True, text=True, errors="replace")
    diagnostics = compiled.stdout + compiled.stderr
    target.with_suffix(".log").write_text(diagnostics, encoding="utf-8")
    if compiled.returncode:
        raise RuntimeError(diagnostics)
    return target


def main() -> None:
    OUTPUT.mkdir(parents=True, exist_ok=True)
    dxil = compile_probe(False)
    assembly = subprocess.check_output([str(DXC), "-dumpbin", str(dxil)], text=True)
    (OUTPUT / "hemisphere.dxil.txt").write_text(assembly, encoding="utf-8")
    constants = re.findall(r"call void @dx\.op\.storeOutput\.f32\([^\n]*, float ([^ )]+)\)", assembly)
    if [float(value) for value in constants] != [0.0, 0.0, 0.0, 56.0]:
        raise AssertionError(f"AO hemisphere projection, unit-length or outward-direction failure: {constants}")
    spirv = compile_probe(True)
    validator = shutil.which("spirv-val")
    if validator is None:
        raise RuntimeError("spirv-val must be on PATH for Vulkan shader validation")
    subprocess.run([validator, "--target-env", "vulkan1.3", "--scalar-block-layout", str(spirv)],
                   check=True, capture_output=True, text=True)
    print("AO hemisphere regression passed: 56 cases preserve cosine projection, unit length and outward direction.")
    print("DXIL/Vulkan compilation passed; SPIR-V validation passed.")


if __name__ == "__main__":
    main()
