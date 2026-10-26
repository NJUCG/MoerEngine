# Vulkan RHI

## Source responsibilities

| Source | Responsibility |
|---|---|
| `VulkanDevice.cpp` | Device initialization, capabilities, fault handling and resource entry points. |
| `VulkanPipelineCreation.cpp` | Shader binding reflection and graphics/compute pipeline construction; implements the existing device entry points. |
| `VulkanPipeline.cpp` | Pipeline-owned layouts and binding metadata. |
| `VulkanRaytracingResource.cpp` | BLAS/TLAS storage, stable scene instance slots and generation of instance updates. |
| `VulkanNativeQueue.cpp` | Native queue submission and semaphore/label operations. |
| `VulkanQueue.cpp` | Graphics queue execution and shared command preprocessing/recording. |
| `VulkanCopyQueue.cpp` | Copy queue handoff, submission and completion. |
| `VulkanQueueProfiler.cpp` | Timestamp collection and CPU recording profiles. |
| `VulkanSurfaceSupport.cpp` | Swapchain surface support queries. |

The queue split preserves the existing queue, recorded-submit and completion
owners. `VulkanQueueInternal.h` only declares operations shared by the queue
implementations; it introduces no additional runtime owner. Reordering and
explicit resource-state scheduling are unchanged.

Creation calls requiring `VK_SUCCESS` use a check that throws in every build
configuration. Partial pipeline construction releases shader modules, layouts
and the pipeline object on failure. Expected runtime statuses still follow the
existing retry/recreate/fault classification. External-memory creation chains
are local values whose lifetime covers the native allocation call.

Scene instance IDs name stable array slots. Free/reuse updates that same slot;
native instance bytes are generated for each prepared update instead of kept
in a second persistent CPU array. Prepared commands retain their native input
resources through the existing submission lifetime.

## Descriptor storage and binding

The shader-facing set/binding interface is preserved. The device selects native
**descriptor heap > descriptor buffer > descriptor sets** from its capabilities;
ordinary and bindless pipelines both use that backend. Pipeline reflection only
selects traditional sets for concrete fallback constraints: input attachments in
the current implementation or a complete heap push-data layout over the device limit.

Heap pipelines use shader binding mappings and push data. Buffer pipelines may
combine descriptor-buffer offsets with one push descriptor set when the device
supports bufferless push descriptors. Pass authors do not select these APIs.

| Owner | Responsibility |
|---|---|
| `VulkanDescriptorHeap` | Encode/cache native descriptors and own the selected backend's GPU storage; no descriptor byte storage in sets mode. |
| `VulkanDescriptorBufferAllocator` | Reserve byte ranges and recycle them under existing submission or persistent-array lifetime guarantees. |
| `VulkanBindlessArray` | Own the index table and persistent descriptors; maintain compatible traditional sets for pipeline fallback. |
| `VulkanPipelineState` | Retain backend choice, layouts or heap mappings, binding offsets, and immediate-data metadata. |
| `VulkanCommandList` / `VulkanDescriptorBinding.cpp` | Write/bind descriptors and immediate data; own transient native pools and texel views until context completion. |

See [Vulkan descriptor backends](../../../../../docs/VULKAN_DESCRIPTOR_BACKENDS.md)
for selection rules, lifetime guarantees, the cost of maintaining fallback sets,
and the reproducible GPU readback matrix.

## Lifetime boundaries

Resources retain their CPU cache slot references until destruction. A cached
descriptor handle does not own the resource; the resource must remain alive
while its descriptor is copied or used by the GPU.

A submission lease shares descriptor allocation bookkeeping among recorders.
Creating a lease does not reserve GPU storage. Ranges are allocated as needed,
flushed after recording joins, and explicitly recycled after GPU completion
or after all referencing work has been discarded. Dropping a shared pointer
alone does not authorize reuse of GPU storage.

## Validation

`VulkanHeaderContract` checks the shared Vulkan header contract, and
`VulkanDescriptorBufferAllocatorContract` exercises allocation, concurrent
recording, atomic allocation failure, and retirement without requiring a Vulkan GPU.
`VulkanFailureContract` verifies failure propagation with assertions disabled;
`VulkanRaytracingInstanceContract` covers growth, slot reuse and invalid frees.

Integration-test observers, scripted failures, and submission probes live in
`source/test/vulkan`. Ordinary builds exclude their registration state and
runtime branches. On a Vulkan-capable platform, explicitly enable them before
building the integration executable:

```sh
cmake -S . -B build -DMOER_VULKAN_TEST_SUPPORT=ON
cmake --build build --target TestRHIParallelRecordVulkan
```

Set `MOER_VULKAN_TEST_SUPPORT=OFF` (the default) to return to the ordinary build.
An observer's storage must still outlive all observed work; enabling test support
does not change submission ownership or callback lifetime rules.

The macOS Render target currently excludes the Vulkan backend. A successful
macOS engine build or CPU contract test does not validate Vulkan GPU execution.
