"""Compile the production AO history predicates against disocclusion/footprint fixtures.

Runs without a GPU: DXC folds the actual shader helpers to eight expected pass
values. The Vulkan build is also validated, rather than testing a Python copy.
"""

from pathlib import Path
import re
import shutil
import subprocess


REPO = Path(__file__).resolve().parents[2]
DXC = REPO / "3rdparty/dxc_2026_02_20/bin/x64/dxc.exe"
OUTPUT = REPO / "build/shader-regressions/ao-history"


def main() -> None:
    OUTPUT.mkdir(parents=True, exist_ok=True)
    probe = REPO / "tools/shaders/probes/AoHistoryRegression.hlsl"
    common = [str(DXC), str(probe), "-T", "ps_6_8", "-E", "main", "-O3",
              "-I", str(REPO / "shaders"), "-I", str(REPO / "source/runtime/render/shaderheaders")]
    dxil = OUTPUT / "geometry-predicates.dxil"
    subprocess.run(common + ["-DDXIL=1", "-Fo", str(dxil)], check=True, capture_output=True, text=True)
    assembly = subprocess.check_output([str(DXC), "-dumpbin", str(dxil)], text=True)
    (OUTPUT / "geometry-predicates.dxil.txt").write_text(assembly, encoding="utf-8")
    constants = re.findall(r"call void @dx\.op\.storeOutput\.f32\([^\n]*, float ([^ )]+)\)", assembly)
    if len(constants) != 8 or any(float(value) != 1.0 for value in constants):
        raise AssertionError(f"AO geometry/footprint regression: {constants}")

    spirv = OUTPUT / "geometry-predicates.spv"
    subprocess.run(common + ["-DVULKAN=1", "-spirv", "-fspv-target-env=vulkan1.3",
                             "-fvk-use-dx-layout", "-Fo", str(spirv)],
                   check=True, capture_output=True, text=True)
    validator = shutil.which("spirv-val")
    if validator is None:
        raise RuntimeError("spirv-val must be on PATH for the Vulkan regression probe")
    subprocess.run([validator, "--target-env", "vulkan1.3", "--scalar-block-layout", str(spirv)],
                   check=True, capture_output=True, text=True)
    print("AO history passed: world-space identity, depth/normal disocclusion, invalid history, "
          "nearest-pixel footprint, layer separation and packed-normal validity.")


if __name__ == "__main__":
    main()
