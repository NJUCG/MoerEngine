# Vulkan DescriptorHeap Replacement Design

## 1. Scope

This document defines the replacement plan for the current Vulkan `VK_EXT_descriptor_buffer` implementation.

The target is a latest-only Vulkan `VK_EXT_descriptor_heap` path that:

- removes the current descriptor-buffer-based regular binding path
- removes the current Vulkan bindless indirect-buffer protocol
- aligns Vulkan shader access with direct `ResourceDescriptorHeap` and `SamplerDescriptorHeap` semantics
- keeps validation and rollout driven by existing repository PowerShell test scripts

This document intentionally does not preserve the current Vulkan descriptor-buffer path as a compatibility mode.
Once the replacement path is complete, the old path should be deleted.

## Build

Use the repository's existing CMake build flow when validating this migration.

Preferred commands:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang
cmake --build build --config Debug --target TestRHITranslate
cmake --build build --config Debug --target MoerEditor
```

Notes:

- If the workspace already uses the generated `build/` directory, reuse it instead of creating ad-hoc build folders.
- Use existing repository scripts for test execution after build.
- Do not introduce custom one-off build wrappers for this migration.

## 2. Design Rules

This design follows `docs/rules/Rule_Codex.md`:

- do not keep fallback or compatibility implementations
- do not over-design
- validate with tests
- filter validation output and treat errors as blockers
- use PowerShell for Windows test scripts

## 3. Current State Summary

The current Vulkan implementation is descriptor-buffer-centric.

Regular descriptor binding currently depends on:

- `VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT`
- `vkGetDescriptorSetLayoutBindingOffsetEXT`
- `vkCmdBindDescriptorBuffersEXT`
- `vkCmdSetDescriptorBufferOffsetsEXT`
- a global mapped descriptor ring buffer used during command recording

Vulkan bindless currently depends on:

- a separate `bindless_array_buffer` that stores indirect handles
- separate descriptor buffers for bindless textures and buffers
- shader-side indirection through `g__array_114514_bdls`
- CPU-side packing of `sampler_idx | slot` handles

The current device extension path only enables `VK_EXT_descriptor_buffer`.
There is no actual runtime plumbing yet for `VK_EXT_descriptor_heap` feature query, property query, device enablement, heap binding commands, or native descriptor writes.

## 4. Core Conclusion

The replacement is feasible, but only if the work is split into the correct order.

The old document mixed three different problem classes into one sequence:

1. descriptor ring thread-safety cleanup
2. Vulkan API migration from descriptor buffer to descriptor heap
3. bindless semantic migration from stable logical handles to snapshot-local heap indices

These must not be implemented as one flat checkpoint list.

The correct order is:

1. add descriptor-heap runtime capability and supporting metadata
2. replace regular binding with offline-to-online heap copies
3. replace Vulkan bindless ABI with direct heap indexing
4. add dedicated tests for heap lifetime and all binding classes
5. delete the old descriptor-buffer path

## 5. Non-Goals

This design does not:

- preserve `VK_EXT_descriptor_buffer` as a runtime fallback
- keep the Vulkan indirect bindless array protocol alive behind wrappers
- preserve globally stable Vulkan bindless handle semantics across submits or frames
- introduce a generic descriptor framework for future unknown backends

## 6. Current Conflicts That Must Be Resolved

### 6.1 Extension and Feature Plumbing Is Missing

The engine currently wires `VK_EXT_descriptor_buffer`, not `VK_EXT_descriptor_heap`.

Before any migration work begins, the Vulkan device path must gain:

- `VkPhysicalDeviceDescriptorHeapFeaturesEXT`
- `VkPhysicalDeviceDescriptorHeapPropertiesEXT`
- device creation enablement for `VK_EXT_descriptor_heap`
- command loading and validation for:
  - `vkCmdBindResourceHeapEXT`
  - `vkCmdBindSamplerHeapEXT`
  - `vkWriteResourceDescriptorsEXT`
  - `vkWriteSamplerDescriptorsEXT`
  - `vkCmdPushDataEXT`

Without this, deleting the current descriptor-buffer path would only break the backend.

### 6.2 Pipeline Layout and Regular Binding Model Must Be Rebuilt

The current `VulkanPipelineParamBinder` is built around descriptor-buffer offsets and descriptor-set-layout byte offsets.

That model conflicts directly with descriptor heap usage.

The replacement path must remove the following assumptions:

- one global descriptor buffer binding table per command buffer
- byte offsets returned by `vkGetDescriptorSetLayoutBindingOffsetEXT`
- `desc_buffer_offsets` as the main runtime transport
- `vkCmdBindDescriptorBuffersEXT` plus `vkCmdSetDescriptorBufferOffsetsEXT` as the regular binding contract

The new regular binding contract must become:

- offline descriptor payload generation
- online heap allocation for the current command buffer or shared snapshot region
- `vkCmdBindResourceHeapEXT` and `vkCmdBindSamplerHeapEXT`
- push-data mapping per shader stage via `VkShaderDescriptorSetAndBindingMappingInfoEXT`

### 6.3 The Current Global Descriptor Ring Is the Wrong Lifetime Model

The current Vulkan path uses one global mapped descriptor ring buffer with frame-sliced offsets.

This is incompatible with descriptor-heap reserved ranges and with correct in-flight lifetime reuse.

The replacement must move to:

- offline canonical descriptor storage for long-lived CPU-managed descriptors
- online transient heap allocations for command-buffer-local bindings
- online shared snapshot regions for bindless views when multiple command buffers in one submit share the same snapshot
- reclamation based on submit retirement or completion events, not frame index reset

### 6.4 Current Bindless ABI Is Not Direct Heap Indexing

The current Vulkan shader ABI is not direct descriptor heap access.

It still uses:

- an indirection buffer `g__array_114514_bdls`
- per-resource slot protocols
- packed texture plus sampler handles
- CPU-managed remap layers inside `VulkanBindlessArray`

The replacement must change Vulkan to the same conceptual model already used by DXIL:

- shaders access resources directly from `ResourceDescriptorHeap[index]`
- shaders access samplers directly from `SamplerDescriptorHeap[index]`
- Vulkan no longer reconstructs descriptor access through an intermediate array buffer

### 6.5 Image Descriptor Write Path Needs New Metadata

The current image descriptor path depends on `vkGetDescriptorEXT` and cached `VkImageView` objects.

The descriptor-heap write path requires enough metadata to regenerate native image descriptors through `vkWriteResourceDescriptorsEXT`.

That means `VulkanTexture` view storage must preserve:

- `VkImageViewCreateInfo`
- any data required to reconstruct sampled-image and storage-image descriptors
- enough layout-facing metadata to author native descriptor writes without returning to the old descriptor-buffer path

This is a hard prerequisite, not a late optimization.

### 6.6 Existing Tests Do Not Cover the New Failure Modes

Current PowerShell scripts exist, but they do not yet validate descriptor-heap-specific behavior.

Missing coverage includes:

- capability detection and explicit skip behavior for descriptor-heap-only tests
- validation log parsing for the RHI test path
- lifetime tests that prove online heap regions are not reused before GPU completion
- correctness tests for direct bindless snapshot visibility across multiple in-flight submits

## 7. Replacement Architecture

### 7.1 Offline Heaps

Offline heaps are the canonical CPU-managed descriptor registry.

They store persistent descriptor payloads for:

- sampled images
- storage images
- uniform buffers
- storage buffers
- texel buffers
- acceleration structures
- samplers

Rules:

- offline heaps are not bound directly as live shader-visible heaps
- mutable CPU descriptor updates land in offline storage first
- bindless logical slots resolve to offline entries, not directly to online heap offsets

### 7.2 Online Heaps

Online heaps are the live shader-visible descriptor memory ranges bound to command buffers.

They are used for two distinct cases:

- transient regular parameter binding for a single command buffer
- read-only bindless snapshot regions shared by one or more command buffers in the same submit

Rules:

- online allocations must be reclaimed only after GPU completion
- reuse is driven by submit retirement or equivalent completion signaling
- frame-index reset is not sufficient
- reserved ranges required by `VK_EXT_descriptor_heap` must be explicitly tracked and never overlapped incorrectly

### 7.3 Regular Binding Model

Regular binding becomes offline-to-online copy plus push-data mapping.

New flow:

1. pipeline reflection builds descriptor-set-and-binding mapping information for descriptor heap usage
2. command recording resolves bound resources to offline descriptor entries
3. command recording allocates online heap space for the current command buffer
4. descriptor payloads are copied from offline heaps into the online range
5. command recording binds resource and sampler heaps
6. command recording emits `vkCmdPushDataEXT` for the mapped set and binding metadata

The old descriptor-buffer offset transport must be deleted.

### 7.4 Bindless Model

Bindless becomes snapshot-based direct heap indexing.

New flow:

1. CPU-side bindless allocation resolves each logical slot to an offline descriptor entry
2. command recording discovers the bindless slots referenced by the submit or command buffer
3. the runtime builds an online bindless snapshot containing the required descriptors
4. CPU-side structures that upload bindless indices now upload snapshot-local online indices
5. shaders access the online heap directly through `ResourceDescriptorHeap` or `SamplerDescriptorHeap`

The current Vulkan indirect handle array must be deleted.

### 7.5 ABI Rule

Vulkan bindless indices are no longer globally stable handles.

They become snapshot-local indices.

Implications:

- GPU scene uploads that previously stored long-lived bindless handles must be regenerated or remapped when snapshot indices change
- material and draw packet payloads must stop assuming lifetime-stable Vulkan bindless indices
- any runtime code that tries to emulate the old stable-handle meaning through another layer should be rejected

## 8. Implementation Stages

### Stage 1: Runtime Capability and Metadata Foundation

Required work:

- add `VK_EXT_descriptor_heap` extension plumbing to the Vulkan device extension system
- query descriptor-heap features and properties
- load all required function pointers and expose them through the backend command path
- add image-view create-info caching for native descriptor writes
- define new resource usage and heap binding abstractions for resource heaps and sampler heaps

Completion requirement:

- the engine can detect whether descriptor heap is supported and can fail or skip cleanly when unsupported

### Stage 2: Replace Regular Binding

Required work:

- replace descriptor-buffer-specific pipeline layout creation with descriptor-heap-native shader mapping
- replace global descriptor ring allocation with online heap allocation
- replace descriptor-buffer offset binding with resource/sampler heap bind plus push-data
- keep the runtime limited to one clear binding path instead of dual execution modes

Completion requirement:

- graphics and compute pipelines can bind all regular parameter classes without touching the descriptor-buffer path

### Stage 3: Replace Vulkan Bindless ABI

Required work:

- delete Vulkan indirect bindless array protocol
- make Vulkan bindless snapshots resolve directly to online heap indices
- update CPU-side consumers to upload snapshot-local indices
- update shader-side Vulkan bindless macros to direct heap access
- align Vulkan semantics with the existing DXIL direct-heap model

Completion requirement:

- Vulkan shader code no longer depends on `g__array_114514_bdls` for descriptor indirection

### Stage 4: Add Binding and Lifetime Tests

Required work:

- extend RHI test coverage to all regular descriptor binding classes
- add direct bindless snapshot tests
- add online heap retirement tests
- add mixed tests for regular bindings and bindless snapshots in the same execution path

Completion requirement:

- descriptor-heap tests fail on correctness regressions and on validation errors

### Stage 5: Delete Old Path

Required work:

- delete descriptor-buffer-specific pipeline flags and layout assumptions
- delete `vkCmdBindDescriptorBuffersEXT` and `vkCmdSetDescriptorBufferOffsetsEXT` recording path
- delete Vulkan indirect bindless array buffer protocol
- delete `vkGetDescriptorEXT`-based regular binding path
- delete any now-dead storage and helper functions that only exist for the old model

Completion requirement:

- the Vulkan backend has one descriptor path only

## 9. Required Code Changes by Area

### 9.1 Vulkan Extension and Device Layer

Files likely affected:

- `source/runtime/render/rhi/vulkan/VulkanExtension.cpp`
- `source/runtime/render/rhi/vulkan/VulkanDeviceProperty.h`
- `source/runtime/render/rhi/vulkan/VulkanDevice.cpp`

Changes:

- add descriptor-heap feature and property query support
- expose descriptor-heap function pointers
- create internal heap resources instead of descriptor-buffer-only resources

### 9.2 Pipeline Layout and Reflection Mapping

Files likely affected:

- `source/runtime/render/rhi/vulkan/VulkanRHIResource.cpp`
- `source/runtime/render/rhi/vulkan/VulkanRHIResource.h`
- `source/runtime/render/rhi/vulkan/VulkanDevice.cpp`

Changes:

- replace descriptor-buffer layout creation assumptions
- build shader mapping info for descriptor heap usage
- replace offset-based binder transport with heap-binding plus push-data transport

### 9.3 Descriptor Storage and Allocation

Files likely affected:

- `source/runtime/render/rhi/vulkan/VulkanDescriptor.cpp`
- `source/runtime/render/rhi/vulkan/VulkanDescriptor.h`
- `source/runtime/render/rhi/vulkan/VulkanSubmissionExecutor.cpp`
- `source/runtime/render/rhi/vulkan/VulkanSubmissionRuntime.cpp`
- `source/runtime/render/rhi/vulkan/VulkanInterruptRuntime.cpp`

Changes:

- introduce offline canonical descriptor storage
- introduce online heap allocator and retirement-aware reclamation
- wire heap lifetime to submission completion instead of frame reset

### 9.4 Bindless Runtime and Shader ABI

Files likely affected:

- `source/runtime/render/rhi/vulkan/VulkanRHIResource.cpp`
- `source/runtime/render/rhi/vulkan/VulkanRHIResource.h`
- `source/runtime/render/rhi/vulkan/VulkanQueue.cpp`
- `shaders/core/common/Bindless.hlsl`
- shader parameter headers and CPU upload paths that carry bindless indices
- scene, material, GPU scene, and draw payload code that stores bindless handles

Changes:

- remove Vulkan indirect bindless array protocol
- move to snapshot-local direct indices
- update shader accessors to direct heap access

### 9.5 Image Descriptor Metadata

Files likely affected:

- `source/runtime/render/rhi/vulkan/VulkanRHIResource.cpp`
- `source/runtime/render/rhi/vulkan/VulkanRHIResource.h`
- `source/runtime/render/rhi/vulkan/VulkanDescriptor.cpp`

Changes:

- store enough image view creation metadata to call `vkWriteResourceDescriptorsEXT`
- remove regular dependence on `vkGetDescriptorEXT` for image descriptors

## 10. Test Script Plan

All validation must continue to run through the existing PowerShell test entrypoints under `testscripts/`.

### 10.1 Existing Scripts to Extend

- `testscripts/test_rhi_translate.ps1`
- `testscripts/test_all.ps1`
- `testscripts/common.ps1`

### 10.2 Required Script Behavior Additions

The scripts must gain explicit support for descriptor-heap validation:

- detect whether the current build and runtime environment support the descriptor-heap path
- skip descriptor-heap-only tests with a clear reason when unsupported
- capture stdout and stderr into logs using the existing run folder convention
- parse Vulkan validation output for the RHI test path instead of relying only on process exit code
- treat validation errors as blockers
- keep warnings and info non-blocking unless explicitly promoted
- print a clear pass, fail, or skip result for each descriptor-heap test block

### 10.3 RHI Script Output Rules

`testscripts/test_rhi_translate.ps1` should report:

- executable path
- log path
- capability state for descriptor heap
- pass, fail, or skip for each descriptor-heap test group
- tail output plus extracted failure lines when a test fails

### 10.4 Common Script Utilities

`testscripts/common.ps1` should gain helpers for:

- descriptor-heap capability detection if the executable exposes it
- validation error extraction for the RHI log path
- standardized skip reporting
- grouped summary output for descriptor-heap subtests

## 11. Required DescriptorHeap Binding Tests

The replacement is not complete without explicit tests for all binding classes.

### 11.1 Regular Binding Coverage

The RHI test suite must add descriptor-heap coverage for:

- single uniform buffer binding
- single storage buffer binding
- uniform texel buffer binding
- storage texel buffer binding
- sampled image binding
- storage image binding
- sampler-only binding
- acceleration structure binding where supported
- array descriptors for sampled images
- array descriptors for storage buffers
- mixed set and binding layouts in one pipeline
- push-constant plus descriptor-heap binding in the same draw or dispatch

### 11.2 Graphics and Compute Coverage

Each supported binding class should be exercised in:

- one graphics pipeline path
- one compute pipeline path when the descriptor type is valid for compute

This is necessary because the new heap-binding and shader-mapping path is shared but stage usage is not identical.

### 11.3 Bindless Coverage

The suite must add descriptor-heap bindless tests for:

- bindless sampled textures
- bindless samplers
- bindless storage buffers
- mixed bindless texture plus sampler access
- multiple bindless snapshots in different in-flight submits
- snapshot-local index remap correctness after a later submit publishes a different snapshot

### 11.4 Lifetime and Concurrency Coverage

The suite must add tests proving:

- online heap ranges are not reused before GPU completion
- multiple command buffers in the same submit can share the same bindless snapshot safely
- multiple command buffers using transient regular bindings do not overlap heap regions incorrectly
- later submits do not corrupt descriptors still required by earlier in-flight submits

### 11.5 Mixed Regression Coverage

The suite must also include mixed tests that combine:

- regular descriptor-heap bindings
- bindless descriptor-heap access
- queue submission ordering
- completion-driven heap retirement

This is the minimum coverage needed to catch the real migration risks.

## 12. Acceptance Criteria

The replacement is complete only when all of the following are true:

- Vulkan runtime uses `VK_EXT_descriptor_heap` instead of `VK_EXT_descriptor_buffer`
- regular Vulkan parameter binding no longer uses descriptor-buffer offsets
- Vulkan bindless no longer uses indirect array-buffer descriptor lookup
- Vulkan shaders access descriptor heaps directly
- image descriptor generation no longer depends on the old `vkGetDescriptorEXT` regular path
- online heap reuse is tied to GPU completion instead of frame reset
- PowerShell test scripts can report pass, fail, and skip for descriptor-heap coverage
- RHI tests cover all regular descriptor binding classes plus bindless heap access
- old descriptor-buffer code paths are deleted

## 13. Final Rule

This migration must be implemented as a replacement, not as an extension of the current Vulkan descriptor-buffer architecture.

If a proposed change still depends on:

- descriptor-buffer byte offsets as the main runtime transport
- the Vulkan indirect bindless array protocol
- frame-index-based descriptor lifetime reuse
- a compatibility branch that keeps both descriptor-buffer and descriptor-heap execution paths alive

then the design is still wrong.