# Renderer Threading Validation

These Windows-only tools validate the compiled RDG renderer and the shared
Executor → Translate → Submission → Completion path. The configurable frontend
choices are the Render Thread, its frame-lag limit, and RDG parallel recording.
Every Vulkan scenario uses the same submission executor.

## Tools and Configuration

- `runtime_verify.py` launches one editor process, waits for a renderer-ready
  marker, captures windows, optionally stresses window state, closes the editor,
  and validates its logs.
- `run_matrix.py` creates an isolated TOML config for each scenario and invokes
  the verifier. It does not rewrite the base config or the executable's config.
- `compare_captures.py` compares corresponding captures from two runs.
- `run_parallel_record_vulkan_test.py` runs the compiled-submission integration
  tests independently of editor captures.

`MoerEditor --config <path>` selects a generated config. The matrix defaults to
`template.MoerEngine.toml`; use `--base-config` to choose another complete template.
Generated configs enable `engine.threading.profile_logging` and set the Render
Thread, frame lag, renderer, Raster graph debug dump, and Raster parallel-recording
choices explicitly. All renderer paths use compiled RDG commands; graph debug
output is an observation option, not a switch between submission models.

## Common Commands

Build first:

```powershell
cmake --build build --config Debug --target MoerEditor --parallel 4
```

List scenarios, run the smoke set, or validate generated configs without launching:

```powershell
python tools/threading/run_matrix.py --list
python tools/threading/run_matrix.py --set smoke
python tools/threading/run_matrix.py --set full --dry-run
```

Run complete functional, graph-dump, resource-display, and lifecycle checks:

```powershell
python tools/threading/run_matrix.py --set full --continue-on-failure
python tools/threading/run_matrix.py --set rendergraph --continue-on-failure
python tools/threading/run_matrix.py --set rendergraph-resources --continue-on-failure
python tools/threading/run_matrix.py --set feature --continue-on-failure
```

For repeated steady-state checks, keep the scene fixed and avoid window stress:

```powershell
python tools/threading/run_matrix.py --set soak --repeat 3 --soak-seconds 300
python tools/threading/run_matrix.py --scenario raster_rt1 --repeat 3 --skip-window-stress
python tools/threading/run_matrix.py --scenario ray_rt1 --repeat 3 --skip-window-stress
```

## Scenario Coverage

| Set | Scenarios | What varies |
|---|---|---|
| `smoke` | `raster_gt`, `raster_rt1`, `ray_rt1` | Renderer and frontend ownership |
| `full` | `raster_gt`, `raster_rt0`, `raster_rt1`, `ray_gt`, `ray_rt0`, `ray_rt1` | Game Thread or Render Thread, lag 0 or 1 |
| `rendergraph` | `raster_graph_gt`, `raster_graph_rt0`, `raster_graph_rt1`, `raster_graph_rt1_reload_switch` | Same frontend choices with graph dumps, plus reload/switch |
| `rendergraph-resources` | `raster_key_{base,normal,depth}_{gt,rt1}` | GBuffer selection and GT/RT snapshot ownership |
| `soak` | `raster_rt1`, `ray_rt1` | Long-running RT with lag 1 |
| `fault` | `ray_rt1_fault_present_submit` | Synthetic device loss at Present submit |
| `feature` | Five scenarios below | Recording and renderer lifecycle |

`raster_gt`, `raster_rt1`, `ray_rt1` and their corresponding graph-dump scenarios
also stress window resizing, minimizing and restoring unless disabled by
`--skip-window-stress`. The remaining scenarios isolate their selected behavior.

The feature scenarios are:

- `feature_renderer_switch`: Raster reload, Raster→Raytracing→Raster, and normal
  shutdown with exact renderer-destruction counts.
- `feature_raster_graph_serial_recording`: RT with lag 1 and serial RDG recording.
- `feature_raster_graph_parallel_recording`: the same lifecycle with parallel RDG
  recording enabled.
- `feature_raster_graph_parallel_recording_gt`: parallel RDG recording without a
  Render Thread.
- `feature_raster_graph_parallel_recording_reload_switch`: parallel recording
  across renderer reload and switching.

Lifecycle scenarios terminate themselves after publishing the final marker and
validate exit status and teardown counts. They deliberately omit screenshots and
periodic profiling gates, which may not appear before a fast run exits. Parallel
variants require completed TaskGraph dispatch markers with at least two worker
jobs; a serial fallback does not satisfy the gate.

Resource-display scenarios select `base_color`, `normal` or
`depth_linear_sampler`. Each selection runs once from GT and once with RT lag 1.
Compare corresponding captures to inspect their visual consistency:

```powershell
python tools/threading/compare_captures.py `
  target/validation/rt_rhi/<run>/raster_key_base_gt `
  target/validation/rt_rhi/<run>/raster_key_base_rt1 `
  --output target/validation/rt_rhi/<comparison>/report.json `
  --max-mean-absolute-error 1 --max-rmse 3 --max-p99 10 `
  --min-all-channels-le-2-ratio 0.80
```

The comparator prefers the common `01_maximized.bmp`, requires equal dimensions,
and reports RGB error distribution. Tolerances account for dynamic sampling
between independent processes. A successful matrix run verifies each process;
it does not by itself establish image equivalence.

## Pass Criteria and Evidence

A run passes when its configured GT/RT markers, compiled renderer marker and
Vulkan executor startup marker appear; requested captures succeed and exceed the
nonblack threshold; teardown completes normally; and no unexpected assertion,
Vulkan VUID, device loss, queue failure or tracked-resource residue appears.
Raster runs require one compiled execution marker per renderer instance. Reload
and switch runs require three Raster instances and at least three graph dumps
when dumps are enabled.

Profiled runs require `[ThreadingProfile][Prepare]`, and RT runs additionally
require `[ThreadingProfile][RT]`. Fault and self-terminating lifecycle scenarios
are exempt from the profiling gates. The tool reports only currently produced
RT and PrepareFrame measurements. It does not interpret absent backend recording
samples as zero work.

Each run stores generated config, stdout/stderr, captures and `report.json`.
The root stores `summary.json` and `summary.md` under the default ignored output
folder `target/validation/rt_rhi/`. The runner refuses a nonempty scenario folder
so a retry cannot inherit stale results.

The summary combines the last five one-second profile windows by default:

- RT: prepare cost, task queue latency, render time, GT lag-limit wait and pending
  frame count. Prepare, render and GT-wait measurements use their own sample counts.
- PrepareFrame: window handling, scripting/test/UI hooks, camera state, config
  snapshots, scene updates and snapshots, UI composition and draw-packet copying.
  Workload counters and maxima remain available in the JSON report.

Use `--profile-tail-windows` to choose the steady-state interval. Runs default to
the `Copied ImGui frame includes` ready marker, followed by a 12-second settle
period for scene initialization. Fault runs use one second. Every generated
config is parsed and its requested values checked before launch.

## Fault Validation

Build with `MOER_VULKAN_TEST_SUPPORT=ON` for scripted faults and Vulkan integration
tests. Ordinary builds reject fault-injection arguments; the option defaults to
`OFF`.

```powershell
python tools/threading/run_matrix.py --set fault
```

This passes `--vulkan-fault-inject=present-submit@3`. The verifier exempts only the
fully anchored expected first-fault error, while still checking all other errors.
It requires exactly one injection, first-fault and summary marker. The summary
must show one latched `VK_ERROR_DEVICE_LOST`, no later native submit/present,
rejected later work, skipped command-pool reset, quarantined unsafe resources,
and completed queue synchronization.

Before injecting, the test takes the exclusive native-operation gate, locks each
unique queue and waits for device idle. That is the basis for its
`predrained=true` claim even with dedicated compute or transfer queues.
`--allow-forbidden-log-pattern REGEX` only exempts matching lines;
`--require-log-count REGEX COUNT` verifies an exact marker count.

## Parallel Command Recording

```toml
[engine.render.raster]
# RDG records independent CommandLists on TaskGraph workers.
render_graph_parallel_recording = false
```

The switch currently covers two audited Raster recording groups:
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
