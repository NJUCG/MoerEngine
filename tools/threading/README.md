# RT/RHI Threading Validation

These Windows-only tools turn the editor runtime checks used during the RT/RHI
split into repeatable validation runs.

## Tools

- `runtime_verify.py` launches one `MoerEditor` process, waits for a renderer
  ready marker, captures the main window and platform viewports, optionally
  stresses window state, closes the editor, and validates its logs.
- `run_matrix.py` generates an isolated TOML config for each scenario and runs
  `runtime_verify.py`. It never rewrites the base config or the config next to
  the executable.

`MoerEditor --config <path>` selects the generated config. Launches without
`--config` keep using `MoerEngine.toml` next to the executable.

## Common Commands

Build first:

```powershell
cmake --build build --config Debug --target MoerEditor --parallel 4
```

List scenarios:

```powershell
python tools/threading/run_matrix.py --list
```

Run the three-scenario smoke set:

```powershell
python tools/threading/run_matrix.py --set smoke
```

Run the full functional matrix:

```powershell
python tools/threading/run_matrix.py --set full --continue-on-failure
```

Run repeated threaded RT soak checks for five minutes per process:

```powershell
python tools/threading/run_matrix.py --set soak --repeat 3 --soak-seconds 300
```

Generate and validate all scenario configs without launching the editor:

```powershell
python tools/threading/run_matrix.py --set full --dry-run
```

## Scenario Coverage

| Scenario | Renderer | RT | RHI | Bypass | Lag | Window stress |
|---|---|---:|---:|---:|---:|---:|
| `raster_sync` | Raster | off | off | on | 0 | no |
| `raster_rhi_bypass` | Raster | off | configured | on | 0 | no |
| `raster_rhi_gt` | Raster | off | on | off | 0 | yes |
| `raster_rt0_rhi` | Raster | on | on | off | 0 | no |
| `raster_rt1_rhi` | Raster | on | on | off | 1 | yes |
| `raster_rt1_rhi_off` | Raster | on | off | off | 1 | no |
| `ray_rhi_gt` | Raytracing | off | on | off | 0 | no |
| `ray_rt0_rhi` | Raytracing | on | on | off | 0 | no |
| `ray_rt1_rhi` | Raytracing | on | on | off | 1 | yes |

## Pass Criteria

A run passes only when all of the following are true:

- The expected GT/RT/RHI mode markers appear in the log.
- No default forbidden pattern appears, including assertion, Vulkan VUID,
  device lost, queue submission failure, access violation, or tracked resource
  residue.
- Every requested screenshot succeeds and exceeds the nonblack threshold.
- The process handles `WM_CLOSE`, exits within the timeout, and returns code 0.

Each run writes `stdout.log`, `stderr.log`, screenshots, its generated
`MoerEngine.toml`, and `report.json`. The matrix root also contains
`summary.json` and `summary.md`. The default output root is ignored build output
under `target/validation/rt_rhi/`.

The runner refuses to reuse a non-empty scenario directory so a failed launch
cannot accidentally consume a stale report from an earlier run.

## Notes

- The ready marker defaults to `Copied ImGui frame includes`, which is later
  than HWND creation and avoids sampling an unrendered first frame.
- The base TOML is parsed after scenario values are patched, and all five
  threading/render values are checked before launch.
- `--continue-on-failure` is recommended for a full matrix so one failed
  scenario does not hide the state of later scenarios.
