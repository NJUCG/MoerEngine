#pragma once

#include "AssetTool.h"
#include "rhi/RHI.h"
#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/raster/post_process/ShaderParameters.h"
#include <type_traits>

namespace Moer::Render::Raster {

#define TexHandle       TextureWithHandle
#define E_SAMPLED_COLOR ETextureUsageFlags::SAMPLED | ETextureUsageFlags::COLOR_ATTACHMENT
#define E_C_ATTACH      ETextureUsageFlags::COLOR_ATTACHMENT
#define E_S_DEPTH       ETextureUsageFlags::SAMPLED | ETextureUsageFlags::DEPTH_STENCIL_ATTACHMENT
#define E_S_TRANSFER    ETextureUsageFlags::SAMPLED | ETextureUsageFlags::TRANSFER_DST

#define CUSTOMIZED_SIZE(x, y) Extent2D(x, y)

// 启用超分
// #define SUPER_RESOLUTION_ENABLED WITH_CUDA
// 关闭超分
#define SUPER_RESOLUTION_ENABLED 0

#if WITH_CUDA && SUPER_RESOLUTION_ENABLED
// 超分标记
#define SR_TAG_true  true
#define SR_TAG_false false
#else
// 超分标记
#define SR_TAG_true  false
#define SR_TAG_false false
#endif

#define RASTER_TEXTURES_TABLE_CONFIG                                                                         \
    X(TexHandle, vbuffer, Tex2DTag, TexConfig::Default(PF_R32_UINT).Usage(E_SAMPLED_COLOR).SR(SR_TAG_true))  \
    X(TexHandle,                                                                                             \
      normal,                                                                                                \
      Tex2DTag,                                                                                              \
      TexConfig::Default(PF_A2R10G10B10_UNORM_PACK32).Usage(E_SAMPLED_COLOR).SR(SR_TAG_true))                \
    X(TexHandle,                                                                                             \
      tangent,                                                                                               \
      Tex2DTag,                                                                                              \
      TexConfig::Default(PF_A2R10G10B10_UNORM_PACK32).Usage(E_SAMPLED_COLOR).SR(SR_TAG_true))                \
    X(TexHandle, uv, Tex2DTag, TexConfig::Default(PF_R32G32_SFLOAT).Usage(E_SAMPLED_COLOR).SR(SR_TAG_true))  \
    X(TexHandle,                                                                                             \
      position,                                                                                              \
      Tex2DTag,                                                                                              \
      TexConfig::Default(PF_R32G32B32A32_SFLOAT).Usage(E_SAMPLED_COLOR).SR(SR_TAG_true))                     \
    X(TexHandle,                                                                                             \
      shadow_mask,                                                                                           \
      Tex2DTag,                                                                                              \
      TexConfig::Default(PF_R8_UNORM).Usage(E_SAMPLED_COLOR).SR(SR_TAG_true))                                \
    X(TexHandle,                                                                                             \
      lighting_output,                                                                                       \
      Tex2DTag,                                                                                              \
      TexConfig::Default(PF_R16G16B16A16_SFLOAT).Usage(E_SAMPLED_COLOR).SR(SR_TAG_true))                     \
    X(TexHandle,                                                                                             \
      ao_output,                                                                                             \
      Tex2DTag,                                                                                              \
      TexConfig::Default(PF_R16G16B16A16_SFLOAT).Usage(E_SAMPLED_COLOR).SR(SR_TAG_true))                     \
    X(TexHandle,                                                                                             \
      ao_output_ambient_only,                                                                                \
      Tex2DTag,                                                                                              \
      TexConfig::Default(PF_R8_UNORM).Usage(E_SAMPLED_COLOR).SR(SR_TAG_true))                                \
    X(TexHandle,                                                                                             \
      ao_output_ambient_only_1,                                                                              \
      Tex2DTag,                                                                                              \
      TexConfig::Default(PF_R8_UNORM).Usage(E_SAMPLED_COLOR).SR(SR_TAG_true))                                \
    X(TexHandle,                                                                                             \
      ao_denoiser_accumulate,                                                                                \
      Tex2DTag,                                                                                              \
      TexConfig::Default(PF_R8_UNORM).Usage(E_SAMPLED_COLOR).SR(SR_TAG_true))                                \
    X(TexHandle,                                                                                             \
      ao_denoiser_accumulate_1,                                                                              \
      Tex2DTag,                                                                                              \
      TexConfig::Default(PF_R8_UNORM).Usage(E_SAMPLED_COLOR).SR(SR_TAG_true))                                \
    X(TexHandle,                                                                                             \
      camera_motion_vector,                                                                                  \
      Tex2DTag,                                                                                              \
      TexConfig::Default(PF_R16G16B16A16_SFLOAT).Usage(E_SAMPLED_COLOR).SR(SR_TAG_true))                     \
    X(TexHandle,                                                                                             \
      denoiser_output,                                                                                       \
      Tex2DTag,                                                                                              \
      TexConfig::Default(PF_R16G16B16A16_SFLOAT).Usage(E_SAMPLED_COLOR).SR(SR_TAG_true))                     \
    X(TexHandle,                                                                                             \
      upsample_output,                                                                                       \
      Tex2DTag,                                                                                              \
      TexConfig::Default(PF_R16G16B16A16_SFLOAT).Usage(E_SAMPLED_COLOR))                                     \
    X(TexHandle, ssr_output, Tex2DTag, TexConfig::Default(PF_R16G16B16A16_SFLOAT).Usage(E_SAMPLED_COLOR))    \
    X(TexHandle, aa_texture_1, Tex2DTag, TexConfig::Default(PF_R16G16B16A16_SFLOAT).Usage(E_SAMPLED_COLOR))  \
    X(TexHandle, aa_texture_2, Tex2DTag, TexConfig::Default(PF_R16G16B16A16_SFLOAT).Usage(E_SAMPLED_COLOR))  \
    X(TexHandle, aa_texture_3, Tex2DTag, TexConfig::Default(PF_R16G16B16A16_SFLOAT).Usage(E_SAMPLED_COLOR))  \
    X(TexHandle, aa_texture_4, Tex2DTag, TexConfig::Default(PF_R16G16B16A16_SFLOAT).Usage(E_SAMPLED_COLOR))  \
    X(TexHandle, aa_output, Tex2DTag, TexConfig::Default(PF_R16G16B16A16_SFLOAT).Usage(E_SAMPLED_COLOR))     \
    X(TexHandle,                                                                                             \
      bloom_downsample_chain,                                                                                \
      Tex2DTag,                                                                                              \
      TexConfig::Default(PF_B10G11R11_UFLOAT_PACK32).Usage(E_SAMPLED_COLOR).Mips(6).IndivisualMips())        \
    X(TexHandle,                                                                                             \
      bloom_upsample_chain,                                                                                  \
      Tex2DTag,                                                                                              \
      TexConfig::Default(PF_B10G11R11_UFLOAT_PACK32).Usage(E_SAMPLED_COLOR).Mips(6).IndivisualMips())        \
    X(TexHandle, tonemapping_output, Tex2DTag, TexConfig::Default(PF_R8G8B8A8_UNORM).Usage(E_SAMPLED_COLOR)) \
    X(TexHandle, ui_frame_buffer, Tex2DTag, TexConfig::Default(PF_R8G8B8A8_UNORM).Usage(E_SAMPLED_COLOR))    \
    X(TexHandle, output, Tex2DTag, TexConfig::Default(PF_R8G8B8A8_SRGB).Usage(E_C_ATTACH))                   \
    X(DepthBufferWithHandle,                                                                                 \
      depth_linear_sampler,                                                                                  \
      TexDepthTag,                                                                                           \
      TexConfig::Default(WITH_CUDA ? PF_D32_SFLOAT : PF_D32_SFLOAT_S8_UINT)                                  \
          .Usage(E_S_DEPTH)                                                                                  \
          .SR(SR_TAG_true)                                                                                   \
          .SamplerConfig(SF_LINEAR, SAM_CLAMP_TO_EDGE))                                                      \
    X(DepthBufferWithHandle,                                                                                 \
      depth_nearest_sampler,                                                                                 \
      TexDepthTag,                                                                                           \
      TexConfig::Default(WITH_CUDA ? PF_D32_SFLOAT : PF_D32_SFLOAT_S8_UINT)                                  \
          .Usage(E_S_DEPTH)                                                                                  \
          .SR(SR_TAG_true)                                                                                   \
          .SamplerConfig(SF_NEAREST, SAM_CLAMP_TO_EDGE)                                                      \
          .From(depth_linear_sampler))                                                                       \
    X(TexHandle,                                                                                             \
      noise_tex,                                                                                             \
      Tex2DTag,                                                                                              \
      TexConfig::Asset("noise_256x256.png")                                                                  \
          .Format(PF_R8G8B8A8_UNORM)                                                                         \
          .Usage(E_S_TRANSFER)                                                                               \
          .SamplerConfig(SF_LINEAR, SAM_REPEAT))                                                             \
    X(TexHandle,                                                                                             \
      lut_ggx_emu,                                                                                           \
      Tex2DTag,                                                                                              \
      TexConfig::Asset("LUT/GGX_E_LUT.png")                                                                  \
          .Format(PF_R8G8B8A8_UNORM)                                                                         \
          .Usage(E_S_TRANSFER)                                                                               \
          .SamplerConfig(SF_LINEAR, SAM_REPEAT))                                                             \
    X(TexHandle,                                                                                             \
      lut_ggx_eavg,                                                                                          \
      Tex2DTag,                                                                                              \
      TexConfig::Asset("LUT/GGX_Eavg_LUT.png")                                                               \
          .Format(PF_R8G8B8A8_UNORM)                                                                         \
          .Usage(E_S_TRANSFER)                                                                               \
          .SamplerConfig(SF_LINEAR, SAM_REPEAT))                                                             \
    X(TexHandle,                                                                                             \
      cubemap_tex,                                                                                           \
      TexCubeTag,                                                                                            \
      TexConfig::Asset("Skybox/WaterScene")                                                                  \
          .Format(PF_R8G8B8A8_UNORM)                                                                         \
          .Usage(E_S_TRANSFER)                                                                               \
          .SamplerConfig(SF_LINEAR, SAM_REPEAT))

struct RasterTextures {
    // 批量生成
#define X(TYPE, NAME, TEXTYPE, CONFIG) TYPE NAME;
    RASTER_TEXTURES_TABLE_CONFIG
#undef X

    void CreateFrameBuffers(RenderDevice& device, const uint2& size) {
        // 批量生成
#define X(TYPE, NAME, TEXTYPE, CONFIG)                                                  \
    {                                                                                   \
        TexConfig cfg = (CONFIG);                                                       \
        AssetTool::CreateRasterResource<TEXTYPE>(this->NAME, device, #NAME, size, cfg); \
        LOG_DEBUG(                                                                      \
            "tex {}, size {} x {}",                                                     \
            #NAME,                                                                      \
            (cfg.b_super_resolution ? size.x / 2 : size.x),                             \
            (cfg.b_super_resolution ? size.y / 2 : size.y)                              \
        );                                                                              \
    }
        RASTER_TEXTURES_TABLE_CONFIG
#undef X
    }

    void LoadAndUploadAssets(RenderDevice& device, CommandList& cmd_list) {
        // 批量生成
#define X(TYPE, NAME, TEXTYPE, CONFIG)                                               \
    {                                                                                \
        TexConfig cfg = (CONFIG);                                                    \
        if ((CONFIG).is_asset) {                                                     \
            AssetTool::LoadTexture<TEXTYPE>(device, cmd_list, NAME.tex, cfg, #NAME); \
        }                                                                            \
    }
        RASTER_TEXTURES_TABLE_CONFIG
#undef X
    }

    void AllocateFrameBuffers(CommandList& cmd_list, BindlessArrayRef& bindless_array) {
        // 批量生成
#define X(TYPE, NAME, TEXTYPE, CONFIG)                \
    LOG_DEBUG("Allocating tex handle for {}", #NAME); \
    AssetTool::AllocateRasterResourceHandle(bindless_array, NAME, (CONFIG));
        RASTER_TEXTURES_TABLE_CONFIG
#undef X
        // 提交
        cmd_list.UpdateBindlessArray(bindless_array);
    }

    void FreeFrameBuffers(BindlessArrayRef& bindless_array) {
        // 批量生成
#define X(TYPE, NAME, TEXTYPE, CONFIG) AssetTool::FreeRasterResourceHandle(bindless_array, NAME);
        RASTER_TEXTURES_TABLE_CONFIG
#undef X
    }

    Array<TextureView> GetDisplayableFrameBuffersView() {
        Array<TextureView> views;
        // 批量生成
#define X(TYPE, NAME, TEXTYPE, CONFIG)                                     \
    assert(this->NAME.tex != nullptr && "There is an empty FrameBuffer!"); \
    views.emplace_back(this->NAME.tex->GetView());
        RASTER_TEXTURES_TABLE_CONFIG
#undef X
        // 去除 output
        bool b_has_erased = false;
        for (auto it = views.begin(); it != views.end(); ++it) {
            if (it->GetTexture()->GetName() == "output") {
                b_has_erased = true;
                views.erase(it);
                break;
            }
        }
        assert(b_has_erased && "output not found in views");
        // 返回
        return views;
    }
}; // namespace Moer::Render::Raster

#undef RASTER_TEXTURES_TABLE_CONFIG

#undef CUSTOMIZED_SIZE

#undef TexHandle
#undef E_SAMPLED_COLOR
#undef E_C_ATTACH
#undef E_D_S_ATTACH
#undef E_S_DEPTH

} // namespace Moer::Render::Raster