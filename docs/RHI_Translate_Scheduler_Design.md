# RHI Translate Scheduler Design

## 1. Goals

This design defines a scheduler model for RHI translate that keeps the public scheduling unit at `RHICommandList` granularity while allowing explicit host-side ordering through `GraphEventRef`-backed translate fences.

The design targets three common workloads:

- Parallel draw/dispatch command lists recorded from multiple threads.
- Serial control command lists that mutate shared RHI-side structures such as bindless arrays, descriptor/ring metadata, acceleration-structure build state, and similar control-plane state.
- A frame-end exclusive stage that runs linear RHI tail work only after all prior translate work that must complete for the frame has drained.

## 2. Non-Goals

- No public sub-command-list API in phase 1.
- No fine-grained internal translate slicing in phase 1.
- No second synchronization model parallel to `GraphEventRef`.
- No GPU queue synchronization changes in this design; this document only covers host-side translate scheduling.

## 3. Core Constraints

### 3.1 Scheduler granularity

The scheduler operates on whole `RHICommandList` / `CmdSubmit` translate jobs.

Reasons:

- Keeps scheduling semantics visible to upper layers.
- Avoids hiding ordering decisions inside `VulkanSubmissionExecutor.cpp`.
- Preserves profiling, query, callback, and event ownership on the command-list boundary.

### 3.2 Ownership boundaries

`VulkanSubmissionExecutor.cpp` must remain a batch-level orchestrator only.

Its public producer-side API should reflect queue admission semantics rather than immediate execution semantics:

- rename `Execute(...)` to `Enqueue(...)`
- keep producer access single-threaded
- defer fixed producer-thread affinity until the runtime contract is finalized

It may own:

- input canonicalization (`ExecutorOp`, source submit plans, preprocess snapshots)
- logical dependency extraction from command content
- assembly of translate job descriptors
- hand-off to submission runtime

It must not grow ownership of:

- execution-class scheduling policy
- translate fence chaining policy
- per-job wait resolution against translate frontiers
- translate completion publication
- serial/parallel/frontier state machines

Those responsibilities belong in `VulkanTranslateTask.cpp` (or code owned by that unit) because they are translate-execution concerns, not preprocess concerns.

### 3.3 Runtime split

The design keeps three layers separate:

- Executor/preprocess: build immutable translate jobs.
- Translate task: decide when a translate job may run on the host and publish completion fences.
- Submission runtime: resolve GPU waits/timelines for already translated submissions.

`GraphEventRef`-based translate fences must never be reused as GPU `WaitEvent` / `SignalEvent` objects.

### 3.4 Future TranslateContext policy support

Phase 1 does not expose TranslateContext selection as a public scheduling API, but the scheduler model must preserve enough information for a later policy that decides:

- which `RHICommandList` translate jobs share one `TranslateContext`
- when multiple translate jobs should be merged to reduce platform command-buffer allocation
- when multiple translated jobs should remain separate because parallel translate or submit overlap is measurably beneficial

This requirement exists because two concrete performance metrics already matter:

- minimize platform command-list / command-buffer usage unless parallel benefit is high enough
- minimize submit count when submit merging does not violate ordering or resource-lifetime constraints

The phase-1 scheduler therefore must not bake in any assumption that one `RHICommandList` permanently maps to one `TranslateContext`, one platform command buffer, or one submit.

Instead, the executor/runtime boundary should preserve scheduler-relevant data that can later drive a policy layer, including at least:

- queue affinity
- translated submit boundaries
- explicit host-side ordering requirements
- GPU wait/signal topology already derived by executor submit and sync analysis
- frame-local exclusivity requirements such as frame-end drain behavior

That future policy should be additive to the scheduler, not a rewrite of the scheduler model.

## 4. Execution Classes

Phase 1 uses explicit execution classes chosen by upper layers:

- `Parallel`
- `SerialControl`
- `FrameEndExclusive`

### 4.1 Parallel

Intended for common draw/dispatch command lists.

Default behavior:

- does not wait on the current serial frontier
- may wait on explicit translate fences supplied by the caller
- publishes completion to the parallel frontier and frame frontier

### 4.2 SerialControl

Intended for command lists that touch shared translate-time mutable state.

Default behavior:

- waits for the published parallel frontier
- waits for the current serial frontier
- publishes completion to the serial frontier and frame frontier
- does not automatically block future parallel work unless explicitly published as a fence dependency

This rule is critical: serial work drains earlier work before it starts, but it is not automatically a barrier for all later parallel work.

### 4.3 FrameEndExclusive

Intended for frame-tail logic such as range retirement, descriptor heap retirement, allocator reclamation, statistics flush, and similar linear RHI tail work.

Default behavior:

- waits for all published frame/serial/parallel frontiers required by the frame
- executes exclusively
- resets the frame-local frontier state after completion

## 5. RHI Translate Fence

`RHITranslateFence` is only a thin wrapper over `GraphEventRef`.

It is not a policy object. It does not carry fence kinds, publish modes, or drain flags.
The event is triggered from the translate stage, and preprocess builds later task dependencies against that event.

Current shape:

```cpp
struct RHITranslateFence {
    GraphEventRef event{nullptr};
};
```

## 6. Preprocess Dependency Model

Dependency generation now lives in preprocess.

Preprocess maintains two global chained events:

- `LastFenceEvent`: the aggregated tail of all encountered `TranslateFence` commands.
- `LastTranslateEvent`: the aggregated tail of all translated work that serial-control tasks must wait on.

It also maintains per-source local ordering when needed, and a per-queue translate tail so Vulkan queue translation stays serialized per queue while different queues can still translate in parallel.

Rules:

- `TranslateFence` must follow an earlier translate task in the same command list.
- the fence event waits on that local translate tail, then preprocess folds it into `LastFenceEvent` through a chained `GraphEventRef`
- every later translate task depends on the current `LastFenceEvent`
- non-parallel translate tasks additionally depend on `LastTranslateEvent`
- later lambda tasks in the same source command list depend on the local lambda/translate tail to preserve command-stream order

This removes the old host-side frontier scheduler from `VulkanTranslateTask.cpp`. Translate execution now only dispatches task-graph jobs with already-resolved prerequisite events.

## 7. Ownership Split

`VulkanSubmissionExecutor.cpp` owns:

- preprocess state snapshots
- logical dependency graph extraction
- source submit segmentation
- translate-task dependency assembly
- submit-info assembly

`VulkanTranslateTask.cpp` owns only execution:

- dispatch translate jobs to TaskGraph
- wait on precomputed prerequisite events
- record platform command buffers

- executor builds `QueueTranslateInfo` plus immutable dependency data
- translate task consumes `QueueTranslateInfo` and does not own an extra scheduler state
- submission runtime sees only translated submits and GPU waits

For the streaming version of the design, executor-side admission and progression should use explicit names:

- `Enqueue(...)`: admit new pending work into the streaming preprocess path
- `Flush(RHITranslate)`: guarantee preprocess admission and translate-task emission only
- `Flush(SubmitGPU)`: guarantee preprocess admission plus downstream GPU submit issuance

The execute order is fixed and explicit:

1. `Enqueue(...)` appends batch work to `PreprocessPipe` in FIFO order.
2. `PreprocessPipe` performs segmentation, dependency extraction, and immutable `QueueTranslateInfo` assembly.
3. `PreprocessPipe` emits translate-stage work into `TranslatePipe`.
4. `TranslatePipe` resolves host-side translate waits, records platform command buffers, and enqueues `SubmitInfo` batches into submission runtime.
5. `Flush(RHITranslate)` waits through step 3 for the current pipe frontier.
6. `Flush(SubmitGPU)` additionally waits through step 4 and drains the submission-runtime intake queue.

## 8. Test Specification

### 8.1 Structured log contract

`TestRHITranslate.exe` must emit structured per-case results in log lines:

```text
[TESTCASE][PASS] CaseName
[TESTCASE][FAIL] CaseName :: reason
[TESTCASE][SKIP] CaseName :: reason
```

PowerShell scripts parse these markers and register subtests.

This keeps the executable as the source of truth for case-level status while allowing `test_rhi_translate.ps1` and `test_all.ps1` to aggregate results consistently.

### 8.2 Mandatory phase-1 test coverage

The translate test suite must cover at least:

- command list queue binding
- multi-command-list submit ordering on the same queue
- multi-stage translate/readback correctness
- concurrent descriptor heap range allocation
- graphics/copy-scope round-trip correctness
- multiple copy scopes in one command list
- unknown-first-use copy-scope upload
- present plus copy-scope path
- present plus normal graphics path

### 8.3 Future scheduler-specific cases

When the scheduler is implemented, add cases for:

- `parallel -> serial -> serial -> parallel -> serial`
- serial completion not implicitly blocking later parallel work
- explicit parallel wait on selected serial fence
- frame-end exclusive draining all published frontiers
- shared `TranslateContext` policy reducing platform command-buffer count without violating execution order
- submit-merging policy reducing submit count without changing readback-visible correctness
- negative case: missing explicit wait produces deterministic failure/assert in test build

## 9. Validation Rule

Every translate scheduler change must be validated through existing repository scripts only:

- `testscripts/test_rhi_translate.ps1`
- `testscripts/test_all.ps1`

If a new executable is introduced later, `test_all.ps1` must be extended to aggregate it, but the structured test-case marker contract should remain the same.