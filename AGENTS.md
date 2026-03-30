# MoerEngine — Project Guidelines

## Build & Run

```
just b              # Build (Debug, 30 threads, Ninja + Clang)
just r              # Run MoerEditor
just br             # Build + Run
just gbr            # Generate + Build + Run (full rebuild)
just b Release      # Release build
just clean          # Remove build/ and target/
```

- CMake generator: **Ninja**, compiler: **Clang** (C++20)
- Shader compiler: **DXC** with `-O3 -spirv -fspv-target-env=vulkan1.3 -fvk-use-dx-layout -all-resources-bound`, target `ps_6_0` / `cs_6_0`
- Shaders are compiled at runtime, cached in `asset/shader_cache/{platform}.sdc`
- Output: `target/bin/{Config}/MoerEditor.exe`

## Architecture Overview

```
source/
  editor/                        # ImGui editor app
    main.cpp, Editor.cpp/h       # Entry point, editor lifecycle
    EditorUI.cpp/h               # Main UI layout
    raster_ui/                   #   Raster pipeline UI controls (RasterUI.cpp/h)
    raytracing_ui/               #   RT pipeline UI controls
  runtime/
    core/                        # Math, containers, platform, logging, task graph
      include/math/              #   Vector, Matrix (Base.h, Matrix.h)
      include/misc/              #   Traits.h (C++ ↔ HLSL type aliases: float3, uint4, float4x4)
    Engine.cpp/h                 # Engine init/loop
    render/
      renderer/
        Renderer.cpp/h           # Top-level renderer (selects raster or RT)
        raster/                  # ★ RASTER PIPELINE (most frequently modified)
          RasterRenderer.cpp/h   #   Frame loop: calls each pass in order
          RasterResource.h       #   Per-frame resources (textures, buffers, CSM data)
          RasterTextures.h       #   GBuffer / RT texture definitions
          RasterConfig.h         #   UI-driven config struct (shadow, AO, AA settings)
          DirectionalShadowMaskPass.cpp/h  # Shadow Mask pass
          ShadowDepthPass.cpp/h  #   CSM depth rendering (cascade setup, frustum split)
          LightingPass.h         #   Deferred lighting pass
          GeometryPass.h         #   Geometry / GBuffer pass
          AoPass.h               #   SSAO / RTAO / SSDO
          AaPass.h               #   SMAA / FXAA
          SkyboxPass.cpp/h       #   Skybox rendering
          TonemappingPass.h      #   Tonemapping + bloom
        raytracing/              # RT pipeline (ReSTIR DI, path tracing)
        common/ui/               # ImGui renderer integration
      rhi/                       # ★ RHI ABSTRACTION LAYER
        RHI.h, RHIResource.h     #   Core types (BufferRef, TextureRef, EBufferUsageFlags)
        RHICommon.h              #   Enums (EBufferUsageFlags, EPixelFormat, etc.)
        RHICommand.h             #   Command list / draw dispatching
        vulkan/                  #   Vulkan backend
          VulkanDevice.cpp/h     #     Pipeline creation, descriptor set layout, reflection
          VulkanQueue.cpp/h      #     Command submission, draw/dispatch recording
          VulkanRHIResource.cpp/h #    Buffer/Texture creation, enum translation
          VulkanDescriptor.cpp/h #    Descriptor pool & set management
        d3d12/                   #   D3D12 backend (partial)
      shader/
        ShaderPipeline.h         # ★ Pipeline class macros (DEFINE_SHADER_CONSTANT_STRUCT, etc.)
        ShaderCompiler.cpp/h     #   DXC invocation, SPIR-V generation
        ShaderManager.cpp        #   PSO caching, hot-reload
      shaderheaders/shared/      # ★ C++/HLSL SHARED HEADERS (dual-language via #ifdef __cplusplus)
        raster/
          ShaderParameters.h     #   Raster shared params entry (includes sub-headers)
          SharedEnum.h           #   Shared enums (EShadowMapMode, ERtaoSampleMode, etc.)
          lighting_pass/ShaderParameters.h  # ★ LightingData, pass param structs
      scene/                     # Scene graph, GPU scene, camera, lights
        GpuScene.cpp/h           #   GPU-side scene buffers (instances, materials, lights)
      resources/                 # Vertex factories, mesh resources
```

```
shaders/                                # HLSL shader sources
  core/
    common/
      Bindless.hlsl                     # ★ Bindless heap definition (ArrayBuffer, TextureHandle, BINDLESS_BINDINGS)
      Common.hlsl                       #   Shared utilities (WorldPosFromDepth, packing, etc.)
  materials/
    Brdf.hlsli                          # PBR BRDF (GGX, multi-scatter)
    Material.hlsli                      # Material fetching
  pipelines/
    RasterCommon.hlsli                  # Raster shared helpers
    raster/deferred/
      geometry/                         # GBuffer shaders
      lighting/
        Lighting.hlsli                  # Light accumulation
        RasterLightingPass.frag.hlsl    # Deferred lighting entry point
        shadows/                        # ★ SHADOW SYSTEM
          ShadowMask.frag.hlsl          #   Shadow mask entry (full-screen pass)
          Shadows.hlsli                 #   Shadow dispatch (CSM / point)
          CSM.hlsli                     #   Cascade selection, blend ratio
          PCSS.hlsli                    #   PCSS blocker search + penumbra
          PCF.hlsli                     #   PCF filtering
          ShadowCore.hlsli              #   Bias, blocker stats
          ShadowSampling.hlsli          #   Poisson disk, rotation, sampling utils
      env_and_atmo/                     # Skybox shaders
    postprocess/
      lighting_effects/                 # AO (SSAO, RTAO, SSDO), SSR
      aa/                               # SMAA, FXAA
      denoise/                          # Bilateral, RTAO denoiser
      color/                            # Tonemapping, bloom
      common/                           # Upsample, copy pass
```

## Key Patterns

### Bindless Architecture
All resources go through a unified bindless heap. Shaders declare `BINDLESS_BINDINGS(BufferSpace, TextureSpace, SamplerSpace, AccelSpace)` and access resources by uint handles via `ArrayBuffer(handle).Load<T>()` and `TextureHandle(handle).Sample2D<T>()`.

### Shared C++/HLSL Headers
Files in `shaderheaders/shared/` use `#ifdef __cplusplus` guards:
- C++ side: `namespace Moer::Render`, types from `Traits.h` (`float3` = `Vector3f`, `float4x4` = `Matrix4x4f`)
- HLSL side: `namespace Moer`, native HLSL types
- Both sides see the same struct layout. **Alignment must be manually kept in sync** (see pitfalls below).

### Pipeline Definition (C++ side)
```cpp
class MyPipeline : public RasterPipeline {
    DEFINE_RASTER_PIPELINE_CLASS(MyPipeline);
    DEFINE_SHADER_BUFFER(lighting_data);             // [[vk::binding(N, S)]] ConstantBuffer / StructuredBuffer
    DEFINE_SHADER_CONSTANT_STRUCT(MyParam, param);   // [[vk::push_constant]]
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);              // Bindless heap
    DEFINE_SHADER_ARGS(lighting_data, bdls, param);  // Arg order must match Gfx() call order
};
```
Pass code calls `cmd_list.Gfx(pipeline, bufferRef, bdls, paramStruct)` — argument order matches `DEFINE_SHADER_ARGS`.

### Pass Lifecycle
`RasterRenderer::Render()` calls each pass's `Process()` in order. Each pass owns its pipeline and fills its param struct from `RasterContext`.

## Shadow System Details

- CSM: up to `MAX_CSM_CASCADES` (currently 4) cascades, 4096² shadow maps
- PCSS: 16 blocker + 16 PCF samples, `[unroll]` loops, Poisson disk sampling
- Shadow mask is a separate full-screen pass (`DirectionalShadowMaskPass`) writing to a single-channel texture, read later by `RasterLightingPass`
- `LightingData` is bound as a **UBO (ConstantBuffer)** to the shadow mask shader for hardware-assisted scalarization (fields only enter registers when accessed, not all at once)

## Pitfalls & Lessons Learned

### Register Pressure from Large Struct Loads
**Problem:** `ByteAddressBuffer.Load<LargeStruct>()` loads the ENTIRE struct into vector registers (vgpr) at once. With `LightingData` (~600 bytes), this consumed ~150 live registers, causing 98% register-limited stalls.
**Root cause:** Dynamic array indexing (e.g., `world2shadow_clip[cascade_index]`) forces the compiler to keep all array elements alive simultaneously.
**Solution:** Use `ConstantBuffer<T>` (UBO) instead of `ByteAddressBuffer` for uniform data. The GPU has a dedicated **constant cache** (separate from registers) — fields are fetched via scalar loads (`s_buffer_load`) and shared across all lanes without per-lane register cost.

### cbuffer / std140 Array Padding
**Problem:** In `ConstantBuffer` (std140 layout), each element of a scalar array (e.g., `float[4]`, `uint[4]`) is padded to 16 bytes. A C++ struct with `float cascade_split_ratios[4]` = 16B but GPU sees 64B.
**Solution:** Use vector types instead: `float4 cascade_split_ratios`, `uint4 cascade_shadow_map`. HLSL supports `float4[i]` indexing, so shader code doesn't change.

### Shadow-Specific Optimizations (Applied)
- Sky pixel early-out: `if (depth < 1e-6) return 1.0` (reverse-Z, sky = 0)
- Full-blocker early-out in PCSS: skip PCF if all 16 blocker samples are shadow
- Cascade blend early-out: only sample next cascade when `blend_ratio > 0`
- Redundant texture sample removal in `get_single_shadow()`
- `tan(acos(x))` → `sqrt(1-x²)/x` in slope-scaled bias

### `[unroll]` vs `[loop]` with Static Arrays
Do NOT convert `[unroll]` to `[loop]` when the loop body indexes a `static const` array (e.g., Poisson disk). With `[loop]`, the array must be dynamically indexed, which pushes it into registers or local memory — registers **increase** instead of decrease.

### Depth Format
D32_FLOAT is sufficient (no need for D32_FLOAT_S8_UINT); stencil is unused by the shadow system. Saves VRAM and bandwidth.

### Push Constants Size Limit
Vulkan push constants are limited to 128 bytes on most hardware. Keep pass param structs small — remove fields that can be loaded from buffers instead.

## C++ Type Reference
| HLSL Type | C++ Type | Size | Align |
|-----------|----------|------|-------|
| `float3`  | `Vector3f` (12B, union of 3 floats) | 12 | 4 |
| `float4`  | `Vector4f` (16B) | 16 | 4 |
| `float4x4`| `Matrix4x4f` (64B, union of 4×Vector4f) | 64 | 4 |
| `uint`    | `uint32_t` | 4 | 4 |
| `uint4`   | `Vector4ui` | 16 | 4 |

Note: C++ side has **no implicit padding** (all 4-byte aligned). But **HLSL ConstantBuffer (std140) adds padding** to arrays of scalars — always use vector types for arrays in shared structs.
