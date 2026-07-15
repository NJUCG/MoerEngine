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

Matrix configs set `engine.threading.profile_logging=true`. The regular and
template configs keep it disabled, so normal editor runs do not emit profiling
windows.

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
python tools/threading/run_matrix.py --set smoke --base-config template.MoerEngine.toml
```

Run the full functional matrix:

```powershell
python tools/threading/run_matrix.py --set full --continue-on-failure --base-config template.MoerEngine.toml
```

Run the isolated synthetic device-loss scenario:

```powershell
python tools/threading/run_matrix.py --set fault --base-config template.MoerEngine.toml
```

The `fault` set passes `--vulkan-fault-inject=present-submit@3` to the editor.
It is deliberately separate from `full`, which remains the original nine
non-fault functional scenarios.

Run repeated threaded RT soak checks for five minutes per process:

```powershell
python tools/threading/run_matrix.py --set soak --repeat 3 --soak-seconds 300 --base-config template.MoerEngine.toml
```

Generate and validate all scenario configs without launching the editor:

```powershell
python tools/threading/run_matrix.py --set full --dry-run --base-config template.MoerEngine.toml
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
| `ray_rt1_rhi_fault_present_submit` | Raytracing | on | on | off | 1 | no |

## Pass Criteria

A run passes only when all of the following are true:

- The expected GT/RT/RHI mode markers appear in the log.
- No default forbidden pattern appears, including assertion, Vulkan VUID,
  device lost, queue submission failure, access violation, or tracked resource
  residue.
- Every requested screenshot succeeds and exceeds the nonblack threshold.
- The process handles `WM_CLOSE`, exits within the timeout, and returns code 0.

The fault scenario keeps the default forbidden scan active and exempts only the
fully anchored, expected `[VulkanFault][First]` error line. Injection and summary
lines receive no exemption. It requires exactly one of each marker. The
first-fault line must identify the synthetic fault injected at the third
Present-submit attempt as `VK_ERROR_DEVICE_LOST`, including its queue, timeline,
and work serial. Before publishing that fault, the injector takes the exclusive
native-operation gate, locks every unique queue, and uses `vkDeviceWaitIdle` to
make `predrained=true` safe even when compute or transfer has a dedicated queue.
The summary must prove that the first fault was latched once, the latched result
was `VK_ERROR_DEVICE_LOST`, no native submit or present occurred afterward, at
least one later logical operation was rejected, command-pool reset was skipped,
failed allocators were quarantined, and all queue `Sync` paths completed.

For focused verifier runs, `--allow-forbidden-log-pattern REGEX` exempts only
matching lines from forbidden-pattern checks; it does not disable checks for
other lines. `--require-log-count REGEX COUNT` requires an exact regex match
count across `stdout.log` and `stderr.log`. Both options may be repeated.

Each run writes `stdout.log`, `stderr.log`, screenshots, its generated
`MoerEngine.toml`, and `report.json`. The matrix root also contains
`summary.json` and `summary.md`. The default output root is ignored build output
under `target/validation/rt_rhi/`.

The summary aggregates the last five one-second profile windows by default.
RHI data includes caller cost, enqueue-to-start latency, backend work time,
Execute/Present splits, maximum queue depth, and pending GPU timeline distance.
RT data includes frame prepare, task queue latency, render execution, GT
lag-limit wait, and maximum pending frames. Use `--profile-tail-windows` to
change the steady-state window.

The runner refuses to reuse a non-empty scenario directory so a failed launch
cannot accidentally consume a stale report from an earlier run.

## Notes

- The ready marker defaults to `Copied ImGui frame includes`, which is emitted
  after the first copied draw packet is consumed. The runner then waits 12
  seconds by default so asynchronous scene and ProbeGI initialization can
  reach a visually stable frame before sampling. The fault scenario uses a
  one-second settle because its first two real presents already provide the
  frame captured after the third synthetic submit fault.
- The base TOML is parsed after scenario values are patched, and all five
  threading/render values are checked before launch.
- `--continue-on-failure` is recommended for a full matrix so one failed
  scenario does not hide the state of later scenarios.
