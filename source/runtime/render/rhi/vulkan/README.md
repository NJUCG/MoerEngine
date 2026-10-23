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

The native RHI binding path uses `VK_EXT_descriptor_buffer`. It does not
allocate, update, or bind `VkDescriptorSet` objects. `VkDescriptorSetLayout`
and `VkPipelineLayout` still define the shader's set/binding interface and
the layout of descriptors in memory.

| Owner | Responsibility |
|---|---|
| `VulkanDescriptorHeap` | Cache native descriptor bytes on the CPU; own the mapped GPU descriptor buffer used for pipeline argument bindings. |
| `VulkanDescriptorBufferAllocator` | Reserve buffer ranges on demand and recycle them after submission completion or discarded recording. |
| `VulkanBindlessArray` | Own the bindless index table and the buffer, image, and sampler descriptor storage; track resource membership and updates. |
| `VulkanPipelineState` | Retain set layouts, binding offsets, argument mappings, and push constant metadata. |
| `VulkanCommandList` | Copy argument descriptors into reserved ranges, bind descriptor buffers, set per-set offsets, and push constants. |

CPU descriptor caches are shared by ordinary argument binding and bindless
updates. Cache slot indices are independent of shader binding numbers and
GPU byte offsets. Native descriptor sizes come from device properties;
binding offsets come from the descriptor set layout queries.

Bindless resources use three GPU buffers:

- A plain index table, read by device address passed through push constants.
- A buffer descriptor array.
- Image and sampler descriptor regions in a shared descriptor buffer.

Only the latter two contain descriptors and are bound as descriptor buffers.
The index table maps shader handles to descriptor slots and sampler indices.

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
recording, rollback, and retirement without requiring a Vulkan GPU.
`VulkanFailureContract` verifies failure propagation with assertions disabled;
`VulkanRaytracingInstanceContract` covers growth, slot reuse and invalid frees.

The macOS Render target currently excludes the Vulkan backend. A successful
macOS engine build or CPU contract test does not validate Vulkan GPU execution.
