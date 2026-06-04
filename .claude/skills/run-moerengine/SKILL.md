---
name: run-moerengine
description: Build, run, test, and verify MoerEngine — a real-time Vulkan rendering engine with raster and ray-tracing pipelines. Use to build after code changes, run smoke tests, launch the editor, or validate that the engine starts without crashes or Vulkan validation errors.
---

# Run MoerEngine

MoerEngine is a real-time rendering engine (C++20, Vulkan 1.3) with an ImGui-based editor.
All paths below are relative to the repo root (`F:/Github_Data/MoerEngine/`).

The driver is `.claude/skills/run-moerengine/driver.py`. Use it for all agent-driven workflows.

## Prerequisites

- Windows 10/11 with Vulkan-capable GPU
- Clang 22.x (`F:/LLVM_22.1.4/bin/clang++`)
- Ninja (`/f/Ninja/ninja`)
- CMake 3.26+ (`/f/CMake/bin/cmake`)
- Vulkan SDK 1.3 (`F:/VulkanSDK/1.4.341.1/`)
- Python 3.12+ (`/c/Python312/python`)

Build directory: `build/clang-debug/` (Ninja + Clang, Debug).  
Config file: `MoerEngine.toml` — controls default scene path and render method.  
Output: `target/bin/{Config}/MoerEditor.exe`.

## Build

```bash
python .claude/skills/run-moerengine/driver.py build
```

Or directly with cmake when `just` is unavailable:

```bash
cmake --build build/clang-debug --config Debug --target MoerEditor TestRHITranslate -j20
```

## Run (agent path)

The driver has three stages. Run them independently or as a pipeline:

```bash
# Full smoke: build → test → launch
python .claude/skills/run-moerengine/driver.py all

# Individual stages
python .claude/skills/run-moerengine/driver.py build
python .claude/skills/run-moerengine/driver.py test --filter RHI
python .claude/skills/run-moerengine/driver.py launch --timeout 30
```

### What `test` does

Runs every `Test*.exe` in `target/bin/{Config}/`. Each test executes from its own bin directory so
`MoerEngine.toml` and `asset/` paths resolve. Exit code and structured `[TESTCASE][PASS/FAIL]`
markers are parsed. Logs land in `logs/driver_test_{timestamp}/`.

### What `launch` does

1. Starts `MoerEditor.exe` with stdout/stderr redirected to files.
2. Waits `--timeout` seconds (default 20).
3. Kills the process, scans logs for crash keywords (`[error] ... crash | exception | fatal | access.violation | assert`).
4. Copies any `.dmp` minidumps to the run directory.
5. Copies `MoerEngine.log` from the bin directory and scans for Vulkan validation VUIDs.

A clean launch shows `[LAUNCH] PASSED`. Vulkan validation messages are captured but do not cause
a failure by default — the agent should inspect them and decide.

### Direct invocation (library path)

For changes to core/rhi/rendergraph that don't need the full editor, build and run the relevant
test target directly:

```bash
# Build just the RHI translate test
cmake --build build/clang-debug --config Debug --target TestRHITranslate -j20

# Run it (from its bin dir so config/assets resolve)
cd target/bin/Debug && ./TestRHITranslate.exe
```

## Run (human path)

Launch the editor interactively (requires a display + GPU):

```bash
./target/bin/Debug/MoerEditor.exe
```

**Controls:**
- `OpenScene` button → file picker → load `.gltf` scene
- Right-click drag → rotate camera, `W/A/S/D/Q/E` → move
- `F` → roaming mode
- Camera settings in `Camera` panel (top-right)
- `default_render_method` in `MoerEngine.toml` switches between `"Raster"` and `"Raytracing"`

The editor loads the scene specified by `[engine.scene].scene_path` in `MoerEngine.toml`.
Startup takes up to 60 seconds for large scenes — wait for the viewport to render before
judging a crash.

## Gotchas

- **`just` is not in PATH.** The project's `template.justfile` documents build commands, but `just`
  is not installed. Use `cmake --build build/clang-debug` directly.
- **Build directory is `build/clang-debug`, not `build`.** The CMake configure step used
  `-G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++` which generates a separate
  build tree. The root `build/` directory contains a `vscode/` stub.
- **Tests must run from BIN_DIR.** Test executables look for `MoerEngine.toml` and
  `asset/shader_cache/` relative to their working directory. The driver `cd`s into
  `target/bin/{Config}/` before launching each test.
- **Editor log is in `MoerEngine.log` inside the working directory**, not stdout. The driver
  copies it from `target/bin/{Config}/logs/MoerEngine.log` after the editor exits.
- **Vulkan validation layer (`VK_LAYER_KHRONOS_validation`) may produce known false positives.**
  Known-unavoidable VUIDs from third-party code (glfw, assimp) are not engine bugs. Focus on
  VUIDs that reference engine-owned Vulkan objects.
- **Scene file must exist** at the path in `MoerEngine.toml` → `[engine.scene].scene_path`,
  otherwise the editor starts with an empty scene.
- **Shader cache** lives in `asset/shader_cache/{platform}.sdc`. Corrupt cache → delete the file
  and restart.
- **Renderer toggle** in `MoerEngine.toml`: set `default_render_method = "Raster"` when working
  on the raster pipeline, `"Raytracing"` for RT. The RT pipeline requires a ray-tracing capable GPU.
- **`clang-cl` is not supported.** The project requires pure Clang (not the MSVC-compatible driver).
- **`TestStringSystem` fails to compile** due to a Clang/MSVC `std::apply` incompatibility with
  `wchar_t` lambdas in `source/runtime/core/include/string/Format.h:314`. This is pre-existing;
  the driver builds targets individually so this doesn't block the rest of the build.
- **`TestDxRhi` crashes at startup** (access violation) — the D3D12 backend is partial and
  may not be functional on all systems. Focus on `TestRHITranslate` for Vulkan validation.
- **Vulkan loader "failed to open JSON file" errors** from RenderDoc, Wegame, Epic Games, OBS
  are noise from other installed apps — not engine bugs. The engine VUIDs (e.g.
  `vkCmdBindResourceHeapEXT`) are the ones to watch.
- **`PresentRoundTrip` test is flaky** — may fail with exit=1 depending on GPU timing.
  This is a known issue, not a regression from RHI changes.

## Troubleshooting

| Symptom | Fix |
|---|---|
| `cmake --build build/clang-debug` fails with "ninja: error: unknown target" | Use `--target MoerEditor` or `--target TestRHITranslate` explicitly |
| `MoerEditor.exe` not found | Check `--config` matches: `Debug` goes to `target/bin/Debug/` |
| Editor exits immediately with no log | Run from terminal directly to see console output: `./target/bin/Debug/MoerEditor.exe` |
| "Workspace Path contains non-ASCII characters" | Move the repo to a path without Chinese/Unicode characters |
| Vulkan validation spew at startup | Expected — the engine creates many Vulkan objects. Check `MoerEngine.log` for new VUIDs introduced by recent changes |
| Test times out | Some GPU tests take >60 s. Use `--timeout 120` for launch or run the specific test directly |
| Shader compilation fails | DXC is bundled in `target/bin/Debug/dxc.exe`. Check `asset/shader_cache/` is writable |
