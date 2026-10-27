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
`run_matrix.py` defaults to the tracked `template.MoerEngine.toml`; pass
`--base-config` only when intentionally testing another complete template.

Matrix configs set `engine.threading.profile_logging=true`. The regular and
template configs keep it disabled, so normal editor runs do not emit profiling
windows. They also set `engine.render.raster.render_graph` and
`render_graph_debug_dump` plus `render_graph_parallel_recording` explicitly for every scenario. The existing
functional sets keep the RenderGraph path off; the dedicated `rendergraph` set
enables both the graph and its one-time debug dump.

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

Run the self-terminating optional-feature lifecycle checks:

```powershell
python tools/threading/run_matrix.py --set feature --base-config template.MoerEngine.toml
```

The `feature_renderer_switch` scenario uses the linear Raster path and validates
Raster reload, Raster-to-Raytracing, Raytracing-to-Raster, normal shutdown, and
the exact renderer destruction counts. It is intentionally independent of the
dedicated RenderGraph validation set so it can gate every optional-feature
build without coupling feature coverage to RenderGraph coverage. It gates on
functional lifecycle markers rather than periodic profiling markers, which are
not guaranteed to be emitted before a fast Release validation run exits.

The Raster recording feature checks form an isolated correctness matrix. Both
`feature_raster_graph_serial_recording` and
`feature_raster_graph_parallel_recording` use RT + threaded RHI; only the upper
`render_graph_parallel_recording` switch differs. The
`feature_raster_graph_parallel_recording_gt` variant exercises the same upper
worker handoff without a Render Thread. The
`feature_raster_graph_parallel_recording_reload_switch` variant keeps upper
recording enabled while recreating Raster and switching Raster→Raytracing→Raster,
covering snapshot ownership across renderer teardown. All four leave the lower
`engine.threading.parallel_recording` switch off and use the Raster-only exit
or renderer-switch completion marker, so they validate RDG recording ownership,
join behavior, and normal shutdown without coupling the result to the separate
lower parallel translator. Parallel variants also require the graph dump to
classify `HiZBuild`, `DirectionalShadowMask`, `Lighting`, and `Skybox` as
`parallel-record`, and require completed TaskGraph dispatch markers for both
two-pass production groups. A silent no-TaskGraph serial fallback therefore
fails the feature gate.

Run the full functional matrix:

```powershell
python tools/threading/run_matrix.py --set full --continue-on-failure --base-config template.MoerEngine.toml
```

Run the seven-scenario Raster RenderGraph matrix:

```powershell
python tools/threading/run_matrix.py --set rendergraph --continue-on-failure --base-config template.MoerEngine.toml
```

This covers the same Raster RT/RHI/bypass/lag combinations as `full`, but with
the graph and graph debug dump enabled. It also contains a validation-only
Raster reload, Raster-to-Raytracing switch and Raytracing-to-Raster switch.
`full` remains the original nine functional scenarios and exercises the linear
Raster fallback.

The reload/switch scenario emits its final marker and requests a normal editor
shutdown itself. `runtime_verify.py --expect-exit-after-ready` then validates
the exit code, four render-queue drains, four renderer destructions, three
Raster destructor completions and one Raytracing destructor completion without calling `PrintWindow`
on a window that has crossed several renderer lifetimes. That lifecycle-only
scenario intentionally produces no capture; the other graph scenarios retain
the visual and resize/restore coverage.

Compare the corresponding maximized captures from graph-off and graph-on runs:

```powershell
python tools/threading/compare_captures.py `
  target/validation/rt_rhi/<linear-run>/raster_sync `
  target/validation/rt_rhi/<graph-run>/raster_graph_sync `
  --output target/validation/rt_rhi/<comparison>/report.json `
  --max-mean-absolute-error 1 `
  --max-rmse 3 `
  --max-p99 10 `
  --min-all-channels-le-2-ratio 0.80
```

The comparator deterministically prefers the common `01_maximized.bmp`,
requires equal dimensions, reports RGB error distribution and exits nonzero
when an enabled threshold fails. The nonzero tolerance accounts for dynamic
sampling between two independent editor processes; retain the JSON report with
the validation evidence, not in Git.

Run paired graph-off/on captures for the key GBuffer textures:

```powershell
python tools/threading/run_matrix.py --set rendergraph-resources --continue-on-failure
```

This eight-scenario set forces the editor display to `base_color`, `normal`, and
`depth_linear_sampler`, with one synchronous linear and graph run for each
texture. A second `base_color` pair runs with RT + threaded RHI to cover the
non-empty framebuffer-selection snapshot across GT/RT. Use
`compare_captures.py` on every corresponding pair: the matrix summary validates
each process independently and is not, by itself, a graph-off/on visual-equivalence
result. The validation-only framebuffer flag is strictly limited to those
textures plus `tonemapping_output`; normal launches leave the selection empty
and preserve the UI-controlled framebuffer.

Run the isolated synthetic device-loss scenario:

```powershell
python tools/threading/run_matrix.py --set fault --base-config template.MoerEngine.toml
```

The `fault` set passes `--vulkan-fault-inject=present-submit@3` to the editor.
Build the editor with `MOER_VULKAN_TEST_SUPPORT=ON` for this scenario and for
Ordinary builds reject fault-injection arguments.
It is deliberately separate from `full`, which remains the original nine
non-fault functional scenarios.

Run repeated threaded RT soak checks for five minutes per process:

```powershell
python tools/threading/run_matrix.py --set soak --repeat 3 --soak-seconds 300 --base-config template.MoerEngine.toml
```

The old `--strict-serial-golden` renderer comparison mode is retired and the
runner rejects it. Use `TestRHIParallelRecordVulkan --compiled-commands` for GPU
output comparisons. Low-level legacy recordings may still collect serial
summaries when profiling is enabled; these do not validate the active compiled
renderer path. Barrier summaries consume resolved batches in diagnostics code,
without inferring resource dependencies in the backend.

Generate and validate all scenario configs without launching the editor:

```powershell
python tools/threading/run_matrix.py --set full --dry-run --base-config template.MoerEngine.toml
```

## Scenario Coverage

| Scenario | Renderer | Graph | Dump | RT | RHI | Bypass | Lag | Window stress |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| `raster_sync` | Raster | off | off | off | off | on | 0 | no |
| `raster_rhi_bypass` | Raster | off | off | off | configured | on | 0 | no |
| `raster_rhi_gt` | Raster | off | off | off | on | off | 0 | yes |
| `raster_rt0_rhi` | Raster | off | off | on | on | off | 0 | no |
| `raster_rt1_rhi` | Raster | off | off | on | on | off | 1 | yes |
| `raster_rt1_rhi_off` | Raster | off | off | on | off | off | 1 | no |
| `raster_graph_sync` | Raster | on | on | off | off | on | 0 | no |
| `raster_graph_rhi_bypass` | Raster | on | on | off | configured | on | 0 | no |
| `raster_graph_rhi_gt` | Raster | on | on | off | on | off | 0 | yes |
| `raster_graph_rt0_rhi` | Raster | on | on | on | on | off | 0 | no |
| `raster_graph_rt1_rhi` | Raster | on | on | on | on | off | 1 | yes |
| `raster_graph_rt1_rhi_off` | Raster | on | on | on | off | off | 1 | no |
| `raster_graph_rt1_rhi_reload_switch` | Raster→Raster→Raytracing→Raster | on (Raster) | on | on | on | off | 1 | no |
| `ray_rhi_gt` | Raytracing | n/a | n/a | off | on | off | 0 | no |
| `ray_rt0_rhi` | Raytracing | n/a | n/a | on | on | off | 0 | no |
| `ray_rt1_rhi` | Raytracing | n/a | n/a | on | on | off | 1 | yes |
| `ray_rt1_rhi_fault_present_submit` | Raytracing | n/a | n/a | on | on | off | 1 | no |
| `feature_renderer_switch` | Raster→Raster→Raytracing→Raster | off | off | on | on | off | 1 | no |
| `feature_raster_graph_serial_recording` | Raster | on | on | on | on | off | 1 | no |
| `feature_raster_graph_parallel_recording` | Raster | on | on | on | on | off | 1 | no |
| `feature_raster_graph_parallel_recording_gt` | Raster | on | on | off | on | off | 0 | no |
| `feature_raster_graph_parallel_recording_reload_switch` | Raster→Raster→Raytracing→Raster | on | on | on | on | off | 1 | no |

## Pass Criteria

Vulkan integration tests require a build configured with
`cmake -S . -B build -DMOER_VULKAN_TEST_SUPPORT=ON` before building
`TestRHIParallelRecordVulkan`. Test observers and scripted overrides are excluded
from ordinary builds; this option defaults to `OFF`.

A run passes only when all of the following are true:

- The expected GT/RT/RHI mode markers appear in the log.
- Every non-fault run emits both PrepareFrame and RHI serial-recording profile
  windows. The short synthetic fault run is exempt because it can exit before
  either one-second window completes.
- Raster runs emit exactly one execution-mode marker per Raster renderer
  instance. The reload/switch seam requires exactly three Raster mode markers
  and at least three graph dumps; topology changes may legitimately add another
  unique dump. Graph runs must not emit `[RenderGraph][Fallback]`.
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
RHI serial-recording data is stored in each run's `rhi_record_profile` object.
It includes layer and command counts, measurement-candidate and currently-safe
counts, serial wall and per-command sums, eligible work, modelled critical path,
dispatch/join estimate, predicted net saving, descriptor bytes, barrier/query
counts, calibration tail, topology changes, and command/layer/barrier/descriptor/
query digests. The `golden_*` fields additionally report the semantic serial
contract captured at the actual recording sites: reordered command/layer
shape, emitted barriers, descriptor layout and resource bindings, and timestamp
query events. `golden_complete` plus `golden_incomplete` equals the sampled
submission count; incomplete samples fail closed and are never accepted as a
baseline. Timing and count averages are weighted by `samples`; maxima are
taken across the selected tail windows. `predicted_net_pct` is derived from the
weighted net-saving and serial-wall totals instead of averaging percentages.
For each digest family, `*_digests` contains the sorted unique window rollups,
`*_digest_window_variants` counts those rollups, and `*_variants` is the maximum
number of per-frame variants reported by any selected window. `summary.md`
contains timing, workload/digest-variant, and digest tables for the same data.
RT data includes frame prepare, task queue latency, render execution, GT
lag-limit wait, and maximum pending frames; Prepare, Render, and GT wait use
their own sample counts when tail windows are combined. PrepareFrame data is emitted for
both game-thread and render-thread scenarios. It separates window handling,
scripting/test/UI hooks, camera and test state, config snapshotting, scene
update and snapshot construction, UI composition and draw-packet copying, plus
unattributed time. The report also retains per-window maxima and workload
counters so a timing change can be distinguished from a change in scene or UI
work. PrepareFrame averages are weighted by the `samples` count. Use
`--profile-tail-windows` to change the steady-state window.

For attribution work, keep the scene fixed and run at least three processes per
scenario. Skipping window stress avoids mixing resize/minimize behavior into the
steady-state tail windows:

```powershell
python tools/threading/run_matrix.py --scenario raster_rt1_rhi --repeat 3 --skip-window-stress --base-config template.MoerEngine.toml --outdir target/validation/rt_rhi/prepare_raster
python tools/threading/run_matrix.py --scenario ray_rt1_rhi --repeat 3 --skip-window-stress --base-config template.MoerEngine.toml --outdir target/validation/rt_rhi/prepare_ray
```

Each run's `prepare_profile` object in `summary.json` contains the weighted
breakdown and workload totals. `summary.md` presents the same data in timing and
workload tables. Every non-fault profiled matrix scenario requires at least one
`[ThreadingProfile][Prepare]` and one `[ThreadingProfile][RHIRecord]` window
before it can pass. The short synthetic fault scenario is exempt because it may
terminate before the first one-second profile window is complete.

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
  threading values, the renderer, and all three Raster RenderGraph values are
  checked before launch.
- `--continue-on-failure` is recommended for a full matrix so one failed
  scenario does not hide the state of later scenarios.

## Parallel Command Recording Experiment

There are two independent switches:

```toml
[engine.render.raster]
# Upper topology: RDG batches record independent CommandLists on TaskGraph workers.
render_graph_parallel_recording = false

```

The upper switch currently migrates two audited Raster recording groups:
`HiZBuild` + `DirectionalShadowMask`, then `Lighting` + `Skybox`. Each pass owns
an independent CommandList and an immutable, strongly-owned Prepare snapshot.
The second pair deliberately spans two GPU dependency waves: GPU hazards still
fix Lighting-before-Skybox submission, but do not serialize their CPU command
recording. Token hazards and explicit `DependsOn` edges still split CPU record
groups. `CpuPrepare` is a managed caller-thread join boundary with no command
submission: it may access logical Tokens or `Reference` GPU identities and is
excluded from the GPU queue plan. `ExternalControl` is reserved for
TensorRT/Vulkan-CUDA work that owns its own submit/sync scope. Other mutable
Raster passes remain explicit caller-thread boundaries until their state is
similarly split.

Vulkan records each compiled source in order. RDG pass recording and the RHI
source/native-queue translator retain their existing parallel scheduling. The
retired backend layer recorder has no configuration or fallback path.

Run the compiled submission matrix after building the integration target:

```powershell
python tools/threading/run_parallel_record_vulkan_test.py `
  --executable target/bin/Debug/TestRHIParallelRecordVulkan.exe `
  --outdir target/validation/compiled_submission/debug
```

The matrix covers serial/parallel RDG recording, native data readback, queue
handoffs, failed/rejected submission retirement, bounded pipeline windows,
Present boundaries, raytracing exports, owning readback futures and queries.
Every process also emits one fixed-schema
`[Vulkan][GPU_ENV]` record after complete Vulkan initialization. The record
proves which physical device ran the test and distinguishes validation being
requested, available, and actually enabled.

For a manually operated strict qualification run on a known Windows/NVIDIA
machine, enable the fail-closed policy explicitly:

```powershell
python tools/threading/run_parallel_record_vulkan_test.py `
  --executable target/bin/Debug/TestRHIParallelRecordVulkan.exe `
  --outdir target/validation/gpu-ci/local `
  --strict-gpu-gate `
  --require-vendor-id 0x000010de `
  --require-device-id 0x00002c02 `
  --require-device-type discrete_gpu `
  --minimum-device-api 1.3.0
```

Strict mode requires the same GPU identity in all 21 processes, complete
validation-layer proof, Vulkan 1.3 or newer, and zero `TESTCASE` skips. It
preserves separate stdout/stderr plus a combined log for every mode, including
partial output on timeout, and writes `summary.json` on success. The runner
refuses a non-empty output directory so a failed retry cannot inherit stale
mode logs or a previous PASS summary. This qualification remains a local tool;
the repository does not register it as an automated GitHub Actions PR gate or
require a persistent self-hosted GPU runner.

The ready-native-lane gate accepts a PASS only when the
`G,G,C,Copy` source order produces first-ready `G,C,Copy` Translate lanes and
an actually observed serial Submission-owner order of `G,G,C,Copy`. When Copy
aliases Graphics or Compute, the marker must explicitly report
`first_ready_lanes=alias_fallback`; Graphics and Compute still need distinct
native lanes. A SKIP is limited to queue unavailability or a
Graphics/Compute native alias, must report all three native queue identities,
and must retain the `VulkanTranslateWaveScheduler` CPU seam.

The recoverable and shutdown gates use a Graphics-to-Copy suffix to prove that
Copy translation remains on the Translate owner, native submission remains on
the Submission owner, the readback is verified, callbacks retire exactly once,
and rejected work is neither natively submitted nor replayed. Bounded window-2
overlap falls back to effective window 1 when any available
Graphics/Compute/Copy native-queue pair aliases. Its overlap marker must carry
all three stable queue identities, blocked admission, and the
`RHISubmissionPipelinePolicy` CPU seam. A focused bounded subtest may report
`reason=queue_unavailable` only when its required logical queues are absent;
the runner validates that SKIP separately instead of weakening the remaining
available-queue contracts.
