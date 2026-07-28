---
name: vulkan
description: Vulkan API reference — spec-driven, no guessing, modern/advanced implementations. Always consult the spec before writing Vulkan code.
---

# Vulkan Skill

## Core Rule

**Every Vulkan API detail comes from the specification.** Never guess function signatures, enum values, valid usage, or extension requirements.

## Information Sources (in priority order)

### 1. WebFetch (preferred — renders markdown)

```bash
WebFetch url="https://github.khronos.org/Vulkan-Site/spec/latest/chapters/<chapter>.html"
```

`WebFetch(domain:github.khronos.org)` is pre-authorized. This converts the spec page to markdown, making it much easier to read function signatures and struct definitions than raw HTML.

### 2. Vulkan SDK local headers (fastest, no network)

The Vulkan SDK is at `F:/VulkanSDK/1.4.341.1/`. All Vulkan types, functions, and enums are in:
- `F:/VulkanSDK/1.4.341.1/Include/vulkan/vulkan_core.h` — core 1.3 API
- `F:/VulkanSDK/1.4.341.1/Include/vulkan/vulkan_shared.h` — common types
- `F:/VulkanSDK/1.4.341.1/Include/vulkan/vulkan_beta.h` — beta extensions

Use `grep -A N 'symbol' <header>` for exact function signatures and struct definitions. This is faster than any network request and gives the canonical C declaration.

```bash
# Exact function signature
grep -A10 'VKAPI_ATTR.*vkCmdTraceRaysKHR' F:/VulkanSDK/1.4.341.1/Include/vulkan/vulkan_core.h

# Struct definition
grep -A15 'typedef struct VkStridedDeviceAddressRegionKHR' F:/VulkanSDK/1.4.341.1/Include/vulkan/vulkan_core.h
```

**However**, headers don't contain VUIDs, valid usage rules, extension interactions, or behavioral semantics. For those, use the spec.

### 3. curl raw HTML (fallback, works offline from WebFetch)

```bash
curl -L --max-redirs 5 -A "Mozilla/5.0" --connect-timeout 15 -s \
  "https://github.khronos.org/Vulkan-Site/spec/latest/chapters/<chapter>.html" 2>&1 | \
  grep -A N '<symbol>'
```

Notes:
- The spec pages are LARGE (500KB–2MB raw HTML). Always pipe through grep — never dump the whole page.
- `docs.vulkan.org` is the primary host but `WebFetch` is blocked for it. Use `github.khronos.org` (the GitHub Pages mirror) for both curl and WebFetch.
- Both hosts use Antora server-side rendering — the full spec text IS in the raw HTML, no JS needed. But the HTML is deeply nested; simple `sed '/id="X"/,/<\/div>/p'` patterns fail because the `</div>` closure is often hundreds of lines away.

## Symbol → Page Mapping

Vulkan symbols are NOT all on one page. Use this mapping to fetch the right page:

| Symbol type | Page |
|---|---|
| `vk*KHR` / `vk*EXT` functions | Chapter page matching the feature (see below), or `vk.xml` in the SDK |
| `Vk*` structs (non-extension) | `chapters/resources.html` |
| `Vk*` structs (KHR/EXT extension) | Chapter page for the extension |
| `Vk*FlagBits`, `Vk*Flags` | `chapters/<chapter>.html` of the related feature |
| VUID-* | Search the chapter page that owns the function/struct referenced in the VUID |
| Feature descriptions | `chapters/features.html` |
| Limits (max*, min*) | `chapters/limits.html` |
| SPIR-V / shader details | `chapters/shaders.html`, `chapters/interfaces.html` |

### Feature → Chapter mapping

| Feature | Chapter |
|---|---|
| Ray tracing pipeline, SBT, vkCmdTraceRays*, vkGetRayTracingShaderGroupHandles* | `chapters/raytracing.html` |
| Acceleration structures, BLAS/TLAS, vkCmdBuildAccelerationStructures* | `chapters/accelstructures.html` |
| Ray traversal, TraceRay(), ray queries | `chapters/raytraversal.html` |
| Pipelines (graphics/compute/RT), vkCreate*Pipelines | `chapters/pipelines.html` |
| Descriptors, descriptor sets/layouts | `chapters/descriptors.html` |
| Descriptor buffers (`VK_EXT_descriptor_buffer`) | `chapters/descriptorbuffers.html` |
| Descriptor heaps | `chapters/descriptorheaps.html` |
| Synchronization, barriers, pipeline stages | `chapters/synchronization.html` |
| Dynamic rendering | `chapters/renderpass.html` |
| Command buffers | `chapters/cmdbuffers.html` |
| Images, image layouts, transitions | `chapters/images.html` |
| Buffers, memory, device addresses | `chapters/resources.html` |
| Swapchain / WSI | `chapters/VK_KHR_surface/wsi.html` |
| Mesh shading | `chapters/VK_NV_mesh_shader/mesh.html` |
| Dispatch / compute | `chapters/dispatch.html` |
| Queries | `chapters/queries.html` |
| Copies / blits | `chapters/copies.html` |

### If you don't know which chapter

1. **Check the SDK header first** (`grep` in `vulkan_core.h`) — this confirms the symbol exists and gives the exact signature. The header comments often reference the extension name.
2. **Search the spec with `grep`** — broader patterns work: `grep -l 'symbol'` over the cached pages if you have them, or fetch the likely chapter.
3. **For extension functions**: the extension name IS the chapter — `vkCmdTraceRaysKHR` → `VK_KHR_ray_tracing` → `chapters/raytracing.html`.

## Extraction Patterns That Work

### Get a function signature

```bash
# From local SDK header (fastest)
grep -A10 'VKAPI_ATTR.*vkGetRayTracingShaderGroupHandlesKHR' F:/VulkanSDK/1.4.341.1/Include/vulkan/vulkan_core.h

# From spec page (includes VUIDs and semantics)
curl ... | grep -B2 -A50 'id="vkGetRayTracingShaderGroupHandlesKHR"'
```

### Get a struct definition

```bash
# From SDK header
grep -A15 'typedef struct VkStridedDeviceAddressRegionKHR' F:/VulkanSDK/1.4.341.1/Include/vulkan/vulkan_core.h

# From spec page — search for the anchor then get the listingblock
curl ... | grep -A30 'id="VkStridedDeviceAddressRegionKHR"'
```

### Search for a VUID explanation

```bash
# VUIDs are linked with id="VUID-..." attributes
curl ... | grep -B5 -A20 'id="VUID-vkCmdTraceRaysKHR-None-02691"'
```

### Count matches to verify you're on the right page

```bash
curl ... | grep -c '<symbol>'   # If 0, you're on the wrong page
```

## Workflow: Resolving a Vulkan Question

```
1. Identify the symbol (function, struct, VUID, concept)
2. Map symbol → chapter using the table above
3. SDK header for signature/definition (fast, canonical)
4. If VUID or behavioral semantics needed:
   a. WebFetch(github.khronos.org/.../chapter.html) ← preferred
   b. curl github.khronos.org + grep ← fallback
5. Cite the source in code comments: // Spec: <chapter>.html#<anchor>
```

## Design Rules

1. **No guessing.** If you don't know a Vulkan detail, fetch the spec or check the SDK header. Do NOT infer API behavior from D3D12, Metal, or general GPU intuition.

2. **Prefer modern features.** MoerEngine targets Vulkan 1.3. Use:
   - Dynamic rendering (`VK_KHR_dynamic_rendering`, promoted in 1.3)
   - Descriptor buffers (`VK_EXT_descriptor_buffer`) over descriptor sets
   - Timeline semaphores for CPU-GPU and GPU-GPU sync
   - `VK_KHR_synchronization2` for pipeline barriers
   - `VK_KHR_ray_tracing_pipeline` / `VK_KHR_acceleration_structure` for RT

3. **Validate with VUIDs.** When debugging Vulkan validation errors, look up the exact VUID in the spec. Fetch the chapter page for the function referenced in the VUID prefix.

4. **Cite your sources.** Include spec references in comments:
   ```cpp
   // Spec: raytracing.html#shader-binding-table
   // SBT stride must be a multiple of shaderGroupHandleAlignment
   ```

## Known MoerEngine Vulkan Patterns

- Uses `VK_EXT_descriptor_buffer` — descriptors via `vkGetDescriptorEXT`, not `vkUpdateDescriptorSets`
- Resource heap binding: `vkCmdBindDescriptorBuffersEXT` + `vkCmdSetDescriptorBufferOffsetsEXT` + `vkCmdBindResourceHeapEXT`
- Timeline semaphores via `VulkanSubmissionExecutor` and `FenceManager` (`source/runtime/render/rhi/vulkan/`)
- Present/swapchain: `VK_KHR_swapchain` with mailbox/FIFO
- RT pipeline: extension loaded (`VK_KHR_ray_tracing_pipeline`), `VulkanPipelineState::RT` type ready, `TraceRayCmd` defined but **not yet implemented** in `VulkanQueue.cpp`
- Vulkan SDK: `F:/VulkanSDK/1.4.341.1/`
