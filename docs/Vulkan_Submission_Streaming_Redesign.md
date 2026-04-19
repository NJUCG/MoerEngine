# Vulkan Submission Ordered Streaming Redesign

## 1. Scope

This document defines the future submit-side redesign only.

It follows `docs/rules/Rule_Codex.md`:

- no compatibility path for the old submission model
- no over-design
- keep ownership explicit
- avoid code smell
- keep implementation clean and direct

This document does not redesign:

- interrupt retirement internals beyond the submit-facing contract
- present synchronization policy beyond the submit boundary
- resource state tracking outside submit assembly and submit execution

## 2. Problem Statement

The current submit dependency model is too context-heavy.

Today submit execution still leans on broad context such as:

- batch-wide submission dependency state
- submission-key-based wait resolution
- execution contracts that depend on resolved physical waits too early

This has three concrete costs:

1. `SubmitInfo` is heavier than it should be.
2. `SubmissionRuntime` depends on more global context than necessary.
3. Dependency resolution, queue submission, and completion retirement are harder to separate cleanly.

## 3. Design Goals

1. Keep `SubmitInfo` minimal and submit-facing.
2. Resolve physical waits only inside `VulkanSubmissionRuntime`.
3. Preserve strict submit order after work enters `SubmissionThread`.
4. Allow lazy wait resolution at submit time.
5. Remove batch-global resolved wait state from the executor contract.
6. Keep per-queue state in one indexed owner, not in duplicated fields.
7. Keep `InterruptRuntime` downstream from submit only.

## 4. Non-Goals

1. This design does not introduce DAG-ready out-of-order submit scheduling.
2. This design does not let later submits bypass earlier submits.
3. This design does not move retirement logic back into submission scheduling.
4. This design does not keep legacy submission-key wait resolution as a compatibility layer.

## 5. Key Rule

The model is:

- ordered assembly
- lazy wait resolution
- ordered execution

This is not a round-robin scheduler.
This is not a per-queue out-of-order ready-submit scheduler.

The submission thread may scan ahead and cache readiness.
The submission thread may only flush submits from the current ordered head forward.

## 6. Thread Model

### 6.1 Executor Thread

Owner:

- caller thread of `RHIExecutor::Submit(...)`
- preprocess and translate orchestration
- submit assembly

Responsibilities:

- split translated work into final native-submit-sized units
- assign final stable submit order
- allocate `SyncPointId`
- build logical sync dependencies
- enqueue one ordered submit batch into `VulkanSubmissionRuntime`

Must not do:

- must not resolve `SyncPointId` into `WaitEvent`
- must not allocate queue signal values
- must not choose runtime submit readiness
- must not perform retirement work

### 6.2 Submission Thread

Owner:

- internal worker thread inside `VulkanSubmissionRuntime`

Responsibilities:

- consume ordered submit batches
- maintain queue runtime state
- maintain resolved syncpoint table
- lazily resolve waits when scanning pending submits
- flush ready submits strictly in batch order
- emit native queue submits
- publish resolved syncpoints after successful native submit
- hand completion payloads to `VulkanInterruptRuntime`

Must not do:

- must not reorder submit execution
- must not bypass an earlier not-yet-flushed submit
- must not perform host-visible retirement

### 6.3 Interrupt Thread

Owner:

- internal worker thread inside `VulkanInterruptRuntime`

Responsibilities:

- poll physical completion state
- recycle allocator or present resources
- run callbacks
- emit external signals
- unlock completion graph events after retirement-visible work is complete

Must not do:

- must not resolve syncpoint graph semantics
- must not choose submit order
- must not inject queue waits

## 7. Vulkan Timeline Semantics

This design relies on timeline semaphore semantics only as a submit-time synchronization primitive.

Important clarification:

- Vulkan timeline semaphores allow wait-before-signal submission order.
- This design does not use that capability to justify out-of-order execution.
- The capability only means the submit-time wait model remains valid even when wait resolution is delayed until submission.

The execution policy remains ordered.

## 8. Core Data Model

```cpp
using SyncPointId = uint64;

struct SubmitInfo {
    SubmissionKey        key;
    uint64               submit_seq;
    EQueueType           queue;
    VulkanRecordedSubmit recorded_submit;

    SyncPointId          signal_syncpoint;
    Array<SyncPointId>   wait_syncpoints;
};

struct ResolvedSyncPoint {
    EQueueType queue;
    uint64     timeline_handle;
    uint64     value;
};

struct QueueRuntimeState {
    uint64 timeline_handle = 0;
    uint64 next_signal_value = 1;
};

class SubmissionQueueStateSet {
public:
    QueueRuntimeState& Get(EQueueType queue);
    const QueueRuntimeState& Get(EQueueType queue) const;

private:
    std::array<QueueRuntimeState, static_cast<size_t>(EQueueType::Num)> states{};
};

struct SubmitRuntimeCache {
    bool                     ready = false;
    bool                     submitted = false;
    Array<ResolvedSyncPoint> resolved_waits;
};

struct OrderedBatchRuntimeState {
    Array<SubmitInfo>         submits;
    Array<SubmitRuntimeCache> cache;
    uint32                    next_submit_index = 0;
};
```

Rules:

- every `SubmitInfo` exports exactly one `signal_syncpoint`
- every logical dependency is represented only as `wait_syncpoints`
- `submit_seq` is the final ordered execution contract
- `ResolvedSyncPoint` is publish-only after successful native submit
- queue runtime state must live in one indexed owner, not in duplicated per-queue fields

## 9. Executor Implementation

### 9.1 Step 1: Split into Final Submit Units

The executor must produce one `SubmitInfo` per eventual native queue submit.

Example:

- one source command list becomes `gfx -> copy -> gfx`
- executor must materialize exactly three final submit units

The submission thread must not perform this split.

### 9.2 Step 2: Assign Stable Ordered Sequence

The executor must assign one monotonic `submit_seq`.

`submit_seq` is the final handoff order into `SubmissionThread`.
`SubmissionThread` must preserve this order when flushing submits.

### 9.3 Step 3: Allocate One Exported SyncPoint per Submit

Each final submit unit gets one `signal_syncpoint`.

This logical token means:

- this submit has been emitted successfully to the device timeline

### 9.4 Step 4: Build Logical Wait Dependencies

The executor builds logical dependencies only.

Sources of logical dependencies include:

- cross-queue handoff
- copy-scope handoff
- explicit user sync intent
- ordered root boundary dependencies when needed

These dependencies must be stored only as `SyncPointId` references.

The executor must not produce physical `WaitEvent` packets.

### 9.5 Step 5: Build Ordered Batch

The executor produces one ordered batch:

```cpp
struct OrderedSubmitBatch {
    Array<SubmitInfo> submits;
};
```

There is no per-queue runtime stream contract in the executor output.
There is one ordered list and later lazy resolution.

### 9.6 Executor Pseudocode

```cpp
BuildOrderedSubmitBatch(translated_segments):
    OrderedSubmitBatch batch
    uint64 next_submit_seq = 0

    for segment in translated_segments in final submit order:
        SubmitInfo submit
        submit.key              = segment.key
        submit.submit_seq       = next_submit_seq++
        submit.queue            = segment.queue
        submit.recorded_submit  = MoveRecordedSubmit(segment)
        submit.signal_syncpoint = AllocateSyncPointId()
        batch.submits.push_back(std::move(submit))

    for submit in batch.submits:
        submit.wait_syncpoints = BuildLogicalWaitSyncPoints(submit, batch)

    return batch
```

### 9.7 Executor Validation Points

- `batch.submits[i].submit_seq == i`
- every submit has exactly one `signal_syncpoint`
- no executor-owned dependency structure stores `WaitEvent`
- cross-queue dependencies refer only to producer `signal_syncpoint`

## 10. Submission Runtime Implementation

### 10.1 Owned Mutable State

`VulkanSubmissionRuntime` must own only the state needed for lazy resolve and ordered flush:

```cpp
struct SubmissionSchedulerState {
    UnorderedMap<SyncPointId, ResolvedSyncPoint> resolved_syncpoints;
    SubmissionQueueStateSet                      queue_states;
};
```

There must be no duplicated fields such as:

```cpp
QueueStreamState graphics_stream;
QueueStreamState compute_stream;
QueueStreamState copy_stream;
QueueSignalState graphics_signal;
QueueSignalState compute_signal;
QueueSignalState copy_signal;
```

That shape is code smell because it duplicates same-typed state and scales by copy-paste.

### 10.2 Step 1: Initialize Batch Runtime State

When a batch enters `SubmissionThread`, create one runtime view:

- `submits` copied or moved from the batch
- one cache entry per submit
- `next_submit_index = 0`

### 10.3 Step 2: Scan for Readiness

The thread may scan from `next_submit_index` to the end of the batch.

For each not-yet-ready and not-yet-submitted submit:

1. inspect `wait_syncpoints`
2. try to resolve them through `resolved_syncpoints`
3. if all waits are available, collapse them into final physical waits
4. cache the collapsed waits
5. mark the submit as `ready`

This scan may skip unresolved submits and continue scanning later submits.

This is allowed because scanning is only preparation.
It is not execution.

### 10.4 Step 3: Flush Pending Ready Submits in Order

After scanning, the thread must flush submits strictly from `next_submit_index` forward.

Rules:

- if the current ordered head is not `ready`, flushing stops immediately
- later ready submits must not bypass the current ordered head
- flush continues only while the next ordered head is ready

This is the central policy:

- scan may skip
- execution may not skip

### 10.5 Step 4: Resolve Physical Waits Only at Submit Time

Wait resolution happens only in `VulkanSubmissionRuntime`.

Executor output remains logical.
Submission runtime turns:

- `SyncPointId`
- into `ResolvedSyncPoint`
- and then into physical `WaitEvent`

### 10.6 Step 5: Allocate Queue Signal Value

When flushing one submit:

1. get `queue_state = queue_states.Get(submit.queue)`
2. use `queue_state.next_signal_value` as this submit's signal value
3. increment only after successful native submit emission

### 10.7 Step 6: Publish Resolved SyncPoint

After native submit succeeds:

- publish `submit.signal_syncpoint`
- store `{queue, timeline_handle, signal_value}`

Publishing earlier is invalid.

### 10.8 Step 7: Enqueue Interrupt Completion Task

After native submit succeeds:

- build one physical completion task
- enqueue it to `VulkanInterruptRuntime`

This completion task is downstream-only.
It is not part of logical syncpoint resolution.

### 10.9 Submission Pseudocode

```cpp
RunSubmissionThread():
    while (running):
        OrderedSubmitBatch batch = PopBatch()
        OrderedBatchRuntimeState state = InitBatchRuntimeState(std::move(batch))

        while (!BatchFinished(state)):
            ScanBatchReadiness(state)

            uint32 old_index = state.next_submit_index
            FlushPendingReady(state)

            if (state.next_submit_index == old_index):
                FailFastOnUnresolvableOrderedSubmit()
```

Readiness scan:

```cpp
ScanBatchReadiness(state):
    for i in range(state.next_submit_index, state.submits.size()):
        if (state.cache[i].submitted || state.cache[i].ready):
            continue

        SubmitInfo& submit = state.submits[i]
        auto resolved = TryResolveWaitSyncPoints(submit.wait_syncpoints)
        if (!resolved.ready):
            continue

        state.cache[i].resolved_waits = CollapseWaitsByTimelineMax(resolved.values)
        state.cache[i].ready = true
```

Ordered flush:

```cpp
FlushPendingReady(state):
    while (state.next_submit_index < state.submits.size()):
        uint32 i = state.next_submit_index
        if (!state.cache[i].ready):
            break

        SubmitInfo& submit = state.submits[i]
        auto& cache = state.cache[i]

        QueueRuntimeState& queue_state = queue_states.Get(submit.queue)
        uint64 signal_value = queue_state.next_signal_value

        RuntimeSubmitResult result = SubmitToNativeQueue(
            submit.queue,
            submit.recorded_submit,
            cache.resolved_waits,
            signal_value
        )

        queue_state.next_signal_value += 1

        PublishResolvedSyncPoint(
            submit.signal_syncpoint,
            ResolvedSyncPoint{
                .queue = submit.queue,
                .timeline_handle = result.timeline_handle,
                .value = signal_value,
            }
        )

        interrupt_runtime.EnqueueTask(
            BuildCompletionTask(submit, std::move(result))
        )

        cache.submitted = true
        state.next_submit_index += 1
```

### 10.10 Submission Validation Points

- later ready submits may be scanned but may not be flushed before earlier submits
- `next_submit_index` is the only flush head
- signal values are monotonic per queue
- `signal_syncpoint` is published only after successful native submit
- no submit is emitted out of `submit_seq` order

## 11. Wait Resolution and Collapse

### 11.1 Resolve Logical Waits

```cpp
TryResolveWaitSyncPoints(wait_syncpoints):
    Array<ResolvedSyncPoint> resolved

    for id in wait_syncpoints:
        if (!resolved_syncpoints.contains(id)):
            return { .ready = false }
        resolved.push_back(resolved_syncpoints[id])

    return {
        .ready = true,
        .values = std::move(resolved)
    }
```

### 11.2 Collapse Waits by Timeline Handle

Multiple logical waits may map to the same upstream queue timeline.
Only the maximum value per handle should remain.

```cpp
CollapseWaitsByTimelineMax(resolved_waits):
    UnorderedMap<uint64, uint64> max_value_by_handle

    for wait in resolved_waits:
        max_value_by_handle[wait.timeline_handle] =
            max(max_value_by_handle[wait.timeline_handle], wait.value)

    Array<WaitEvent> final_waits
    for (handle, value) in max_value_by_handle:
        final_waits.push_back(WaitEvent{handle, value})

    return final_waits
```

### 11.3 Wait Validation Points

- two waits on the same timeline handle collapse to one physical wait
- the kept wait value must be the maximum one
- waits on different timeline handles must remain separate
- collapse must happen in `VulkanSubmissionRuntime`, not in the executor

## 12. Interrupt Integration

### 12.1 Input Contract

Each successful native submit that requires retirement must enqueue one interrupt task carrying physical completion payload only.

That payload may include:

- queue completion handle
- allocator retirement state
- callbacks
- signal events
- completion graph event

It must not include logical syncpoint graph semantics.

### 12.2 Interrupt Processing Steps

1. poll physical readiness using non-blocking timeline or fence query
2. perform allocator completion
3. run callbacks
4. emit external signals
5. unlock completion graph event

### 12.3 Interrupt Pseudocode

```cpp
RunInterruptThread():
    while (running):
        task = TryAcquireReadyTask()
        if (task == null):
            SleepShort()
            continue

        ResolveAllocatorCompletion(task)
        RunCallbacks(task)
        EmitSignals(task)
        UnlockCompletionEvent(task)
```

### 12.4 Interrupt Validation Points

- interrupt must not resolve syncpoints
- interrupt must not add queue waits
- completion graph event unlock must happen after callback and readback-visible completion work

## 13. Failure Conditions

These are internal logic failures and must fail fast:

1. a submit references a syncpoint that can never be resolved
2. a full scan plus flush cycle makes no forward progress
3. one `SyncPointId` is published twice
4. queue runtime state is missing for a submit queue
5. native submit success path returns without a publishable completion

These are not compatibility cases.
They indicate a broken submit graph or broken runtime ownership.

## 14. End-to-End Example

Example chain:

- `gfx0`
- `copy0` waits on `gfx0.signal_syncpoint`
- `gfx1` waits on `copy0.signal_syncpoint`

Executor output:

```cpp
submits = [
    gfx0(seq=0, wait=[]),
    copy0(seq=1, wait=[SP_gfx0]),
    gfx1(seq=2, wait=[SP_copy0]),
]
```

Execution behavior:

1. first scan marks `gfx0` ready
2. first flush submits `gfx0` and publishes `SP_gfx0`
3. second scan marks `copy0` ready
4. second flush submits `copy0` and publishes `SP_copy0`
5. third scan marks `gfx1` ready
6. third flush submits `gfx1`

This is ordered streaming:

- readiness can be discovered lazily
- submit execution remains strictly ordered

## 15. Checkpoints and Where to Look

### CP-01: Shrink `SubmitInfo` to ordered submit contract

Goal:

- remove submission-context-wide execution dependency shape
- keep only ordered submit identity plus logical syncpoint dependencies

Where to look:

- [VulkanSubmissionShared.h](f:/Github_Data/MoerEngine/source/runtime/render/rhi/vulkan/VulkanSubmissionShared.h)
  - `SubmitInfo`
  - `SubmissionKey`
  - `RootRhiBoundary`
- [VulkanSubmissionExecutor.cpp](f:/Github_Data/MoerEngine/source/runtime/render/rhi/vulkan/VulkanSubmissionExecutor.cpp)
  - submit assembly path
  - `AssembleSubmitInfos(...)`
  - any code still filling `wait_submission_keys`
- [VulkanSubmissionRuntime.h](f:/Github_Data/MoerEngine/source/runtime/render/rhi/vulkan/VulkanSubmissionRuntime.h)
  - `SubmissionBatch`

What to change:

- replace `wait_submission_keys`-style execution contract with `submit_seq + signal_syncpoint + wait_syncpoints`
- keep `SubmitInfo` submit-facing only
- remove any field that exists only to support old full-context wait resolution

### CP-02: Convert executor output to ordered submit batch

Goal:

- executor hands one ordered submit list to submission runtime
- executor no longer hands a queue-ready or pre-resolved wait model

Where to look:

- [VulkanSubmissionExecutor.cpp](f:/Github_Data/MoerEngine/source/runtime/render/rhi/vulkan/VulkanSubmissionExecutor.cpp)
  - final translate-to-submit assembly path
  - queue-segment merge logic
  - present attachment path only as boundary reference
- [VulkanSubmissionRuntime.h](f:/Github_Data/MoerEngine/source/runtime/render/rhi/vulkan/VulkanSubmissionRuntime.h)
  - `SubmissionBatch`

What to change:

- executor must output ordered `Array<SubmitInfo>`
- assign `submit_seq` during final assembly
- assign one `signal_syncpoint` per submit
- attach only logical `wait_syncpoints`
- do not emit physical `WaitEvent` packets from executor

### CP-03: Add submission runtime owned queue state set

Goal:

- remove duplicated per-queue runtime fields
- keep queue runtime state in one indexed owner

Where to look:

- [VulkanSubmissionRuntime.h](f:/Github_Data/MoerEngine/source/runtime/render/rhi/vulkan/VulkanSubmissionRuntime.h)
  - runtime private state block
- [VulkanSubmissionRuntime.cpp](f:/Github_Data/MoerEngine/source/runtime/render/rhi/vulkan/VulkanSubmissionRuntime.cpp)
  - queue signal allocation path
  - submit-to-native queue dispatch path

What to change:

- introduce `SubmissionQueueStateSet`
- move queue timeline handle and `next_signal_value` into that owner
- remove duplicated `graphics_* / compute_* / copy_*` style runtime state if present or if newly introduced during refactor

### CP-04: Implement ordered batch runtime state

Goal:

- submission thread processes one ordered batch with scan cache plus flush head

Where to look:

- [VulkanSubmissionRuntime.h](f:/Github_Data/MoerEngine/source/runtime/render/rhi/vulkan/VulkanSubmissionRuntime.h)
  - batch-facing runtime data definitions
- [VulkanSubmissionRuntime.cpp](f:/Github_Data/MoerEngine/source/runtime/render/rhi/vulkan/VulkanSubmissionRuntime.cpp)
  - `RunSubmissionThread()`
  - any helper currently attaching dependencies or finishing batches

What to change:

- add `OrderedBatchRuntimeState`
- add per-submit cache for `ready / submitted / resolved_waits`
- add `next_submit_index`
- keep this state local to the submission thread path, not in executor

### CP-05: Replace submission-thread scheduling with scan-then-ordered-flush

Goal:

- submission thread may scan ahead
- submission thread may only execute from ordered head forward

Where to look:

- [VulkanSubmissionRuntime.cpp](f:/Github_Data/MoerEngine/source/runtime/render/rhi/vulkan/VulkanSubmissionRuntime.cpp)
  - `RunSubmissionThread()` main loop
  - queue submit scheduling helpers
  - dependency attachment helpers
- [VulkanSubmissionRuntime.h](f:/Github_Data/MoerEngine/source/runtime/render/rhi/vulkan/VulkanSubmissionRuntime.h)
  - helper declarations if needed

What to change:

- add `ScanBatchReadiness(...)`
- add `FlushPendingReady(...)`
- remove any logic that resolves execution readiness through full submission-context-wide dependency walking at submit time
- remove any logic that can execute a later submit before the ordered head submit

### CP-06: Move physical wait resolution fully into submission runtime

Goal:

- only submission runtime resolves `SyncPointId -> WaitEvent`

Where to look:

- [VulkanSubmissionExecutor.cpp](f:/Github_Data/MoerEngine/source/runtime/render/rhi/vulkan/VulkanSubmissionExecutor.cpp)
  - any path that still materializes wait events from logical submit dependencies
- [VulkanSubmissionRuntime.cpp](f:/Github_Data/MoerEngine/source/runtime/render/rhi/vulkan/VulkanSubmissionRuntime.cpp)
  - native submit path
  - queue signal allocation path
  - sync dependency helpers
- [VulkanSubmissionShared.h](f:/Github_Data/MoerEngine/source/runtime/render/rhi/vulkan/VulkanSubmissionShared.h)
  - `WaitEvent`
  - syncpoint-related shared types

What to change:

- add `TryResolveWaitSyncPoints(...)`
- add `CollapseWaitsByTimelineMax(...)`
- resolve logical waits only when scanning or flushing pending submits inside submission runtime
- do not keep executor-side physical wait resolution as fallback

### CP-07: Keep interrupt runtime downstream-only

Goal:

- interrupt runtime handles only physical completion retirement
- logical syncpoint graph ends before interrupt runtime begins

Where to look:

- [VulkanInterruptRuntime.h](f:/Github_Data/MoerEngine/source/runtime/render/rhi/vulkan/VulkanInterruptRuntime.h)
  - `SubmissionCompletionTask`
- [VulkanInterruptRuntime.cpp](f:/Github_Data/MoerEngine/source/runtime/render/rhi/vulkan/VulkanInterruptRuntime.cpp)
  - `RunInterruptThread()`
  - `TryAcquireReadyTask(...)`
  - `IsTaskReady(...)`
  - `ProcessTask(...)`
  - `UnlockTaskCompletion(...)`
- [VulkanSubmissionRuntime.cpp](f:/Github_Data/MoerEngine/source/runtime/render/rhi/vulkan/VulkanSubmissionRuntime.cpp)
  - completion task construction and enqueue path

What to change:

- keep interrupt input as physical completion payload only
- keep callback and readback-visible completion before graph-event unlock
- do not move syncpoint resolve or submit ordering into interrupt runtime

### CP-08: Delete old submission-context-wide resolve path

Goal:

- remove dead compatibility logic after the ordered streaming path is live

Where to look:

- [VulkanSubmissionExecutor.cpp](f:/Github_Data/MoerEngine/source/runtime/render/rhi/vulkan/VulkanSubmissionExecutor.cpp)
  - legacy dependency graph to execution-contract translation
  - old `wait_submission_keys` path
- [VulkanSubmissionRuntime.cpp](f:/Github_Data/MoerEngine/source/runtime/render/rhi/vulkan/VulkanSubmissionRuntime.cpp)
  - old batch dependency attachment helpers
  - old root-boundary-to-submit injection path if it exists only for the legacy model
- [VulkanSubmissionShared.h](f:/Github_Data/MoerEngine/source/runtime/render/rhi/vulkan/VulkanSubmissionShared.h)
  - dead fields kept only for the old execution model

What to change:

- delete old execution dependency transport once ordered streaming is active
- do not keep adapter code that supports both old and new paths
- fail fast on invalid internal state instead of silently falling back

## 16. Migration Plan

1. replace `wait_submission_keys` as the execution contract with `wait_syncpoints`
2. add `submit_seq` and one `signal_syncpoint` to every `SubmitInfo`
3. convert executor output to one ordered submit batch
4. add `SubmissionQueueStateSet` to `VulkanSubmissionRuntime`
5. implement `OrderedBatchRuntimeState`
6. implement scan-then-ordered-flush logic
7. move physical wait resolution fully into `VulkanSubmissionRuntime`
8. keep interrupt runtime downstream-only
9. delete old submission-context-wide resolve code
10. validate with the existing RHI translate test script

## 17. Final Rule

The future submit path must obey this invariant:

- logical dependency construction may happen earlier
- physical wait resolution happens only inside `VulkanSubmissionRuntime`
- readiness may be scanned ahead
- native submit may not execute out of ordered batch sequence

Anything that requires duplicated per-queue state fields, early physical wait resolution in the executor, or out-of-order execution after entering `SubmissionThread` is the wrong design.
