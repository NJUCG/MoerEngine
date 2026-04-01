# RHI Translate Async Pipeline Design (TaskPipe)

## Goals
- Keep `RHIExecutor::Submit` external behavior stable.
- Move CPU work in `VulkanSubmissionExecutor::Execute(Array<RHIExecOp>&&)` to producer-consumer stages.
- Support submit batching from caller side:
  - non-flush submit: cache only
  - flush submit: submit cached batch
  - frame-end marker: attach frame tail event for stats

## Caller API (Flag-Based)
Replace options struct semantics with flags for simpler call sites:

```cpp
enum class ERHIExecSubmitFlags : uint8_t {
    None      = 0,
    FlushGPU  = 1 << 0,
    FrameEnd  = 1 << 1,
};

// Default behavior keeps current immediate submit semantics.
void Submit(Array<RHIExecOp>&& ops,
            ERHIExecSubmitFlags flags = ERHIExecSubmitFlags::FlushGPU);
```

Recommended helper:

```cpp
inline bool HasFlag(ERHIExecSubmitFlags flags, ERHIExecSubmitFlags bit);
```

### Flag Semantics
- `None`: append to pending batch only, do not push to submission thread.
- `FlushGPU`: flush pending batch + current ops to submission thread.
- `FrameEnd`: mark this batch as frame tail.
- `FlushGPU | FrameEnd`: flush and emit frame-end marker in event thread.
- `FrameEnd` without `FlushGPU`: keep frame-end pending until next flush.

## Staged Migration Plan
To avoid breaking call sites, use transitional overload:

```cpp
// Transitional API
void Submit(Array<RHIExecOp>&& ops, RHIExecSubmitOptions options);
```

Internal mapping:
- `options.flush_gpu` -> `FlushGPU`
- `options.frame_end` -> `FrameEnd`

Then gradually migrate callers to the flag API and finally remove the options overload.

## Pipeline Stages
1. `PreprocessStage`
- Input: `Array<RHIExecOp>`
- Output: `PreprocessPacket`
- Responsibilities:
  - do reorder per `RHISubmitCmdList`
  - produce preprocess barrier metadata
  - produce dependency digest inputs

2. `AssembleStage`
- Input: `PreprocessPacket`
- Output: `AssembledPacket`
- Responsibilities:
  - build platform submit/present units
  - bind `submission_key` and queue type

3. `ResolveDependencyStage`
- Input: `AssembledPacket`
- Output: `ResolvedPacket`
- Responsibilities:
  - run `SubmissionDependencyResolver`
  - depend only on read/write hazard digest, not on translate behavior

4. `TranslateStage`
- Input: `ResolvedPacket`
- Output: `TranslatedPacket`
- Responsibilities:
  - call `Queue::Translate(...)` and record Vulkan command buffers
  - no reorder, no preprocess, no restore in this stage
  - parallelizable by submit granularity

5. `SubmitStage`
- Input: `TranslatedPacket`
- Output: `SubmittedPacket`
- Responsibilities:
  - inject wait/signal dependencies
  - call `SubmitRecorded(...)`
  - maintain `submission_key -> completion(wait event)` mapping

6. `PresentEventStage`
- Input: `SubmittedPacket` + present tasks + frame-end marker
- Output: present completion and frame-end stats marker
- Responsibilities:
  - host-wait + present in dedicated event thread
  - avoid blocking submission thread
  - emit frame-end stats hook in marker position

## TaskPipe Mapping
- `PipeA_PreprocessAssemble`: `PreprocessStage -> AssembleStage`
- `PipeB_ResolveTranslate`: `ResolveDependencyStage -> TranslateStage`
- `PipeC_Submit`: `SubmitStage`
- `PipeD_PresentEvent`: `PresentEventStage`

Use `GraphEventRef` to chain stage dependencies. Limit frame in-flight count (suggested: 2~3).

## Data Structures
- `FramePacket`: frame id + per-stage payload
- `SubmitPacket`: `submission_key`, `queue_type`, `recorded_submit`, restore snapshot
- `PresentTask`: `op_seq`, `RHIPresentOp`, `wait_events`
- `FrameEndTask`: batch id + per-frame counters for stats

## Thread Model
- `SubmissionThread`: consumes submit work from `SubmitStage`
- `SubmissionEventThread`: consumes present/frame-end tasks
- allocator creation should move to device-level thread-safe provider (future step)

## Constraints
- `TranslateStage` must not do reorder/preprocess.
- tracker in translate stage must not do restore-state.
- restore transitions are appended by executor after submit on graphics queue.

## Rollout
1. Keep current execution path, add explicit packet/stage boundaries.
2. Introduce flag API on `RHIExecutor::Submit`.
3. Migrate translate stage to parallel pipeline tasks.
4. Move submit/event to dedicated pipes with backpressure metrics.
