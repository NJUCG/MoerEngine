# RHIExecutor Execution Chain

This document summarizes the current Vulkan-side `RHIExecutor` execution flow after the recent submission refactor.

## Entry

Main entry points:

- `RHIExecutor::Submit(...)`
- `VulkanSubmissionExecutor::Execute(...)`
- `VulkanSubmissionExecutor::Flush()`
- `VulkanSubmissionExecutor::Shutdown()`

In the current renderer paths, frame work is usually assembled as:

1. Collect `Array<RHIExecOp>`.
2. Push one or more `RHISubmitCmdList` ops for copy / graphics.
3. Optionally append `RHIPresentOp`.
4. Submit the whole frame through `RHIExecutor`.

## Stage 1: Preprocess Frame Ops

File:

- [VulkanSubmissionExecutor.cpp](F:\Github_Data\MoerEngine\source\runtime\render\rhi\vulkan\VulkanSubmissionExecutor.cpp)

Key function:

- `PreprocessFrameOps(...)`

What happens here:

1. Walk every `RHIExecOp` in submission order.
2. For each `RHISubmitCmdList`, build a `ResourceAccessDigest` with `ResourceAccessCollector`.
3. Chain a frame-local `ResourceStateSnapshot` forward across submits in the same `Execute(...)`.
4. Save:
   - `initial_state_snapshot`
   - `digest`
   - `last_state_snapshot`
   - reordered command cache

Important details:

- `digest` is conservative and tracks read/write intent by resource.
- It now also tracks `last_access_write`, so the last access in the submit is preserved instead of only remembering that a write happened somewhere earlier.
- `write_textures` from `RHISubmitCmdList::MarkWriteTexture(...)` are folded into the last submit in that op.
- Bindless reads are treated conservatively:
  - the bindless array object itself is tracked
  - currently bound contained resources are also promoted into the digest as read dependencies

## Stage 2: Assemble Platform Ops

Key function:

- `AssemblePlatformOps(...)`

This converts high-level `RHIExecOp` into platform-side ops:

- `QueueSubmissionInfo`
- `PresentInfo`

For each submit, it attaches:

- precomputed `digest`
- reordered command cache
- `restore_state_snapshot`

`restore_state_snapshot` currently uses the submit's `last_state_snapshot`.

## Stage 3: Resolve Dependencies

Key type:

- `SubmissionDependencyResolver`

Purpose:

- Build cross-submit / cross-queue wait edges without forcing global queue sync.

Rules:

1. Read-after-write across queues waits on the last writer.
2. Write-after-read across queues waits on all last readers.
3. Present waits on the last writer of the presented texture.
4. Submits after a present in the same op list are marked invalid.

Output:

- `wait_submission_keys` on each `QueueSubmissionInfo`
- `wait_submission_keys` on each `PresentInfo`

## Stage 4: Translate RHI to Vulkan Recorded Submits

Key function:

- `TranslateRHI(...)`

Per queue:

- graphics / compute use `VkCommandQueue::Translate(...)`
- copy uses `VkCopyQueue::Translate(...)`

Translation responsibilities:

1. Reorder commands through `CmdReorderer`.
2. Preprocess commands with `VkCmdPreprocessor`.
3. Record real Vulkan commands through `VkCmdVisitor`.
4. Produce `VulkanRecordedSubmit`.

At this stage command buffers are recorded but not yet submitted.

## Stage 5: Submission Plan Runtime

Key type:

- `SubmissionPlanRuntime`

Threads:

- `SubmissionThread`
- `SubmissionEventThread`

`SubmissionThread` behavior:

1. Pop a `SubmissionBatch`.
2. Execute every `QueueSubmissionInfo` / `PresentInfo` in dependency order.
3. Maintain `completion_by_submit` so later submits/presents can wait on earlier completion events.

## Stage 6: Execute Queue Submit

Key function:

- `SubmissionPlanRuntime::ExecuteSubmit(...)`

Flow:

1. Resolve `wait_submission_keys` into `WaitEvent`s.
2. Append those waits to the recorded submit packet.
3. Submit through:
   - `VkCommandQueue::SubmitRecorded(...)` for graphics / compute
   - `VkCopyQueue::SubmitRecorded(...)` for copy
4. Receive a completion `WaitEvent`.

## Stage 7: Restore Transitions

Current implementation no longer routes restore work through the old `VulkanQueue.cpp` helper path.

Key types:

- `SubmissionRestoreContext`
- `SubmissionRestoreContextManager`

Purpose:

- Submit a small follow-up command buffer that restores resources from the submit's tracked final state back to queue-preferred stable state.

Current behavior:

1. Reconstruct tracked source state from `restore_state_snapshot`.
2. For textures:
   - compute tracked source access/layout from the last state
   - transition back to `texture->GetQueuePreferredLayout(queue)`
3. For buffers:
   - only emit restore barrier if the final tracked access was write-like
4. Submit the restore command buffer on the same queue, waiting on the previous submit completion event.
5. Return a new completion event that replaces the original submit completion.

Allocator lifecycle:

- restore allocators are recycled on a background completion thread
- completion uses `VulkanAllocatorBase::Complete(...)`
- this avoids introducing `HostWait` into arbitrary worker threads

## Stage 8: Present Path

Current present path is also executor-owned.

Key types:

- `SubmissionPresentContext`
- `SubmissionPresentContextManager`

Design goals:

- keep swapchain sync on fence + binary semaphore
- avoid timeline sync inside swapchain objects
- avoid reusing the old `VkCommandQueue::Present(...)` logic directly

Flow:

1. Wait for a reusable swapchain present slot with `vkGetFenceStatus`.
2. Acquire next swapchain image from `VkSwapchain::AquireNextImage()`.
3. Record a present copy command buffer with `VulkanPresentor`.
4. Wait on upstream submit completion events.
5. Wait on acquire semaphore.
6. Signal render-finished binary semaphore.
7. Submit copy command buffer.
8. Call `vkQueuePresentKHR(...)` manually.
9. Recycle `VulkanPresentor` on a background completion thread.

This path eliminated the earlier validation failures around:

- acquire semaphore reuse
- in-flight fence reuse
- freeing pending command buffers
- write-after-present
- device lost during present storm

## Stage 9: Host Wait / Payload Tasks

`SubmissionEventThread` handles:

- host-side waits for explicit wait tasks
- deferred payload callbacks

Current host wait policy:

- waits poll device-visible fence value
- does not call arbitrary `HostWait` from multiple execution threads

## Flush / Shutdown

`VulkanSubmissionExecutor::Flush()` currently flushes:

1. submission runtime
2. restore contexts
3. present contexts

`VulkanSubmissionExecutor::Shutdown()` shuts down in the same order.

## Current Known Weak Point

The remaining validation issue is concentrated on `local_light_pdf_tex` in the raytracing path.

Observed symptom:

- submit expects `VK_IMAGE_LAYOUT_GENERAL`
- validation still thinks current layout is `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL`

Likely cause:

- bindless-driven "write in one pass, sampled in a later pass" still has a gap between real final layout and executor-side final-state reconstruction for this texture chain

Relevant files:

- [PreprocessLightPass.cpp](F:\Github_Data\MoerEngine\source\runtime\render\renderer\raytracing\PreprocessLightPass.cpp)
- [ShaderUtils.cpp](F:\Github_Data\MoerEngine\source\runtime\render\renderer\raytracing\ShaderUtils.cpp)
- [VulkanSubmissionExecutor.cpp](F:\Github_Data\MoerEngine\source\runtime\render\rhi\vulkan\VulkanSubmissionExecutor.cpp)

