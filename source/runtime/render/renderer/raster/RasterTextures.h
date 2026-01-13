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

#define SCREEN_SIZE           Extent2D(size->x, size->y)
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

/**
 * 静态分发处理器：根据持有者类型自动选择创建方法
 */
template<typename IntentTag, typename T_Holder>
void CreateRasterResource(
    T_Holder&     target,
    RenderDevice& device,
    const char*   name,
    const uint2&  size,
    TexConfig&    cfg
) {
    if (cfg.is_asset) {
        // 资源纹理不在这里创建
        return;
    }
    if (cfg.alias_ptr) {
        target.tex = static_cast<T_Holder*>(cfg.alias_ptr)->tex;
    } else {
        if constexpr (std::is_same_v<IntentTag, TexDepthTag>) {
            cfg.type = TexType::TEX_TYPE_DEPTH;
            cfg.dim  = ETextureDimension::TEX_2D;

            target.tex = device.CreateDepthBuffer(
                name,
                (cfg.b_super_resolution ? Extent2D(size.x / 2, size.y / 2) : Extent2D(size.x, size.y)),
                cfg.format,
                1,
                cfg.usage
            );
        } else if constexpr (std::is_same_v<IntentTag, TexCubeTag>) {
            cfg.type = TexType::TEX_TYPE_CUBE;
            cfg.dim  = ETextureDimension::TEX_CUBE;
            cfg.size = {0, 0, 6};

            target.tex = device.CreateCubeMap(
                name,
                (cfg.b_super_resolution ? Extent2D(size.x / 2, size.y / 2) : Extent2D(size.x, size.y)),
                cfg.format,
                cfg.usage,
                cfg.mip_cnt
            );
        } else if constexpr (std::is_same_v<IntentTag, Tex2DTag>) {
            cfg.type = TexType::TEX_TYPE_2D;
            cfg.dim  = ETextureDimension::TEX_2D;

            target.tex = device.CreateTexture(
                name,
                (cfg.b_super_resolution ? Extent2D(size.x / 2, size.y / 2) : Extent2D(size.x, size.y)),
                cfg.format,
                cfg.usage,
                cfg.mip_cnt
            );
        } else {
            static_assert(always_false<T_Holder>, "Unsupported Tex IntentTag");
        }
        LOG_DEBUG(
            "tex {}, size {} x {}",
            name,
            (cfg.b_super_resolution ? size.x / 2 : size.x),
            (cfg.b_super_resolution ? size.y / 2 : size.y)
        );
    }
}

#define RASTER_TEXTURES_TABLE_CONFIG                                                                        \
    X(TexHandle, vbuffer, Tex2DTag, TexConfig::Default(PF_R32_UINT).Usage(E_SAMPLED_COLOR).SR(SR_TAG_true)) \
    X(TexHandle,                                                                                            \
      normal,                                                                                               \
      Tex2DTag,                                                                                             \
      TexConfig::Default(PF_A2R10G10B10_UNORM_PACK32).Usage(E_SAMPLED_COLOR).SR(SR_TAG_true))               \
    X(TexHandle,                                                                                            \
      tangent,                                                                                              \
      Tex2DTag,                                                                                             \
      TexConfig::Default(PF_A2R10G10B10_UNORM_PACK32).Usage(E_SAMPLED_COLOR).SR(SR_TAG_true))               \
    X(TexHandle, uv, Tex2DTag, TexConfig::Default(PF_R32G32_SFLOAT).Usage(E_SAMPLED_COLOR).SR(SR_TAG_true)) \
    X(TexHandle,                                                                                            \
      position,                                                                                             \
      Tex2DTag,                                                                                             \
      TexConfig::Default(PF_R32G32B32A32_SFLOAT).Usage(E_SAMPLED_COLOR).SR(SR_TAG_true))                    \
    X(TexHandle,                                                                                            \
      shadow_mask,                                                                                          \
      Tex2DTag,                                                                                             \
      TexConfig::Default(PF_R8_UNORM).Usage(E_SAMPLED_COLOR).SR(SR_TAG_true))                               \
    X(TexHandle,                                                                                            \
      lighting_output,                                                                                      \
      Tex2DTag,                                                                                             \
      TexConfig::Default(PF_R16G16B16A16_SFLOAT).Usage(E_SAMPLED_COLOR).SR(SR_TAG_true))                    \
    X(TexHandle,                                                                                            \
      ao_output,                                                                                            \
      Tex2DTag,                                                                                             \
      TexConfig::Default(PF_R16G16B16A16_SFLOAT).Usage(E_SAMPLED_COLOR).SR(SR_TAG_true))                    \
    X(TexHandle,                                                                                            \
      ao_output_ambient_only,                                                                               \
      Tex2DTag,                                                                                             \
      TexConfig::Default(PF_R8_UNORM).Usage(E_SAMPLED_COLOR).SR(SR_TAG_true))                               \
    X(TexHandle,                                                                                            \
      ao_output_ambient_only_1,                                                                             \
      Tex2DTag,                                                                                             \
      TexConfig::Default(PF_R8_UNORM).Usage(E_SAMPLED_COLOR).SR(SR_TAG_true))                               \
    X(TexHandle,                                                                                            \
      ao_denoiser_accumulate,                                                                               \
      Tex2DTag,                                                                                             \
      TexConfig::Default(PF_R8_UNORM).Usage(E_SAMPLED_COLOR).SR(SR_TAG_true))                               \
    X(TexHandle,                                                                                            \
      ao_denoiser_accumulate_1,                                                                             \
      Tex2DTag,                                                                                             \
      TexConfig::Default(PF_R8_UNORM).Usage(E_SAMPLED_COLOR).SR(SR_TAG_true))                               \
    X(TexHandle,                                                                                            \
      camera_motion_vector,                                                                                 \
      Tex2DTag,                                                                                             \
      TexConfig::Default(PF_R16G16B16A16_SFLOAT).Usage(E_SAMPLED_COLOR).SR(SR_TAG_true))                    \
    X(TexHandle,                                                                                            \
      denoiser_output,                                                                                      \
      Tex2DTag,                                                                                             \
      TexConfig::Default(PF_R16G16B16A16_SFLOAT).Usage(E_SAMPLED_COLOR).SR(SR_TAG_true))                    \
    X(TexHandle,                                                                                            \
      upsample_output,                                                                                      \
      Tex2DTag,                                                                                             \
      TexConfig::Default(PF_R16G16B16A16_SFLOAT).Usage(E_SAMPLED_COLOR).SR(SR_TAG_false))                   \
    X(TexHandle,                                                                                            \
      ssr_output,                                                                                           \
      Tex2DTag,                                                                                             \
      TexConfig::Default(PF_R16G16B16A16_SFLOAT).Usage(E_SAMPLED_COLOR).SR(SR_TAG_false))                   \
    X(TexHandle,                                                                                            \
      aa_texture_1,                                                                                         \
      Tex2DTag,                                                                                             \
      TexConfig::Default(PF_R16G16B16A16_SFLOAT).Usage(E_SAMPLED_COLOR).SR(SR_TAG_false))                   \
    X(TexHandle,                                                                                            \
      aa_texture_2,                                                                                         \
      Tex2DTag,                                                                                             \
      TexConfig::Default(PF_R16G16B16A16_SFLOAT).Usage(E_SAMPLED_COLOR).SR(SR_TAG_false))                   \
    X(TexHandle,                                                                                            \
      aa_texture_3,                                                                                         \
      Tex2DTag,                                                                                             \
      TexConfig::Default(PF_R16G16B16A16_SFLOAT).Usage(E_SAMPLED_COLOR).SR(SR_TAG_false))                   \
    X(TexHandle,                                                                                            \
      aa_texture_4,                                                                                         \
      Tex2DTag,                                                                                             \
      TexConfig::Default(PF_R16G16B16A16_SFLOAT).Usage(E_SAMPLED_COLOR).SR(SR_TAG_false))                   \
    X(TexHandle,                                                                                            \
      aa_output,                                                                                            \
      Tex2DTag,                                                                                             \
      TexConfig::Default(PF_R16G16B16A16_SFLOAT).Usage(E_SAMPLED_COLOR).SR(SR_TAG_false))                   \
    X(TexHandle,                                                                                            \
      bloom_downsample_chain,                                                                               \
      Tex2DTag,                                                                                             \
      TexConfig::Default(PF_B10G11R11_UFLOAT_PACK32).Usage(E_SAMPLED_COLOR).SR(SR_TAG_false))               \
    X(TexHandle,                                                                                            \
      bloom_upsample_chain,                                                                                 \
      Tex2DTag,                                                                                             \
      TexConfig::Default(PF_B10G11R11_UFLOAT_PACK32).Usage(E_SAMPLED_COLOR).SR(SR_TAG_false))               \
    X(TexHandle,                                                                                            \
      tonemapping_output,                                                                                   \
      Tex2DTag,                                                                                             \
      TexConfig::Default(PF_R8G8B8A8_UNORM).Usage(E_SAMPLED_COLOR).SR(SR_TAG_false))                        \
    X(TexHandle,                                                                                            \
      ui_frame_buffer,                                                                                      \
      Tex2DTag,                                                                                             \
      TexConfig::Default(PF_R8G8B8A8_UNORM).Usage(E_SAMPLED_COLOR).SR(SR_TAG_false))                        \
    X(TexHandle, output, Tex2DTag, TexConfig::Default(PF_R8G8B8A8_SRGB).Usage(E_C_ATTACH).SR(SR_TAG_false)) \
    X(DepthBufferWithHandle,                                                                                \
      depth_linear_sampler,                                                                                 \
      TexDepthTag,                                                                                          \
      TexConfig::Default(WITH_CUDA ? PF_D32_SFLOAT : PF_D32_SFLOAT_S8_UINT)                                 \
          .Usage(E_S_DEPTH)                                                                                 \
          .SR(SR_TAG_true)                                                                                  \
          .SamplerConfig(SF_LINEAR, SAM_CLAMP_TO_EDGE))                                                     \
    X(DepthBufferWithHandle,                                                                                \
      depth_nearest_sampler,                                                                                \
      TexDepthTag,                                                                                          \
      TexConfig::Default(WITH_CUDA ? PF_D32_SFLOAT : PF_D32_SFLOAT_S8_UINT)                                 \
          .Usage(E_S_DEPTH)                                                                                 \
          .SR(SR_TAG_true)                                                                                  \
          .SamplerConfig(SF_NEAREST, SAM_CLAMP_TO_EDGE)                                                     \
          .From(depth_linear_sampler))                                                                      \
    X(TexHandle,                                                                                            \
      noise_tex,                                                                                            \
      Tex2DTag,                                                                                             \
      TexConfig::Asset("noise_256x256.png")                                                                 \
          .Format(PF_R8G8B8A8_UNORM)                                                                        \
          .Usage(E_S_TRANSFER)                                                                              \
          .SR(SR_TAG_false)                                                                                 \
          .SamplerConfig(SF_LINEAR, SAM_REPEAT))                                                            \
    X(TexHandle,                                                                                            \
      lut_ggx_emu,                                                                                          \
      Tex2DTag,                                                                                             \
      TexConfig::Asset("LUT/GGX_E_LUT.png")                                                                 \
          .Format(PF_R8G8B8A8_UNORM)                                                                        \
          .Usage(E_S_TRANSFER)                                                                              \
          .SR(SR_TAG_false)                                                                                 \
          .SamplerConfig(SF_LINEAR, SAM_REPEAT))                                                            \
    X(TexHandle,                                                                                            \
      lut_ggx_eavg,                                                                                         \
      Tex2DTag,                                                                                             \
      TexConfig::Asset("LUT/GGX_Eavg_LUT.png")                                                              \
          .Format(PF_R8G8B8A8_UNORM)                                                                        \
          .Usage(E_S_TRANSFER)                                                                              \
          .SR(SR_TAG_false)                                                                                 \
          .SamplerConfig(SF_LINEAR, SAM_REPEAT))                                                            \
    X(TexHandle,                                                                                            \
      cubemap_tex,                                                                                          \
      TexCubeTag,                                                                                           \
      TexConfig::Asset("Skybox/WaterScene")                                                                 \
          .Format(PF_R8G8B8A8_UNORM)                                                                        \
          .Usage(E_S_TRANSFER)                                                                              \
          .SR(SR_TAG_false)                                                                                 \
          .SamplerConfig(SF_LINEAR, SAM_REPEAT))

struct RasterTextures {
    // 批量生成
#define X(TYPE, NAME, TEXTYPE, CONFIG) TYPE NAME;
    RASTER_TEXTURES_TABLE_CONFIG
#undef X

    void CreateFrameBuffers(RenderDevice& device, const uint2& size) {
        // 批量生成
#define X(TYPE, NAME, TEXTYPE, CONFIG)                                       \
    {                                                                        \
        TexConfig cfg = (CONFIG);                                            \
        CreateRasterResource<TEXTYPE>(this->NAME, device, #NAME, size, cfg); \
        LOG_DEBUG(                                                           \
            "tex {}, size {} x {}",                                          \
            #NAME,                                                           \
            (cfg.b_super_resolution ? size.x / 2 : size.x),                  \
            (cfg.b_super_resolution ? size.y / 2 : size.y)                   \
        );                                                                   \
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
    NAME.handle = bindless_array->AllocateTexture(NAME.tex->GetView(), (CONFIG).sampler);
        RASTER_TEXTURES_TABLE_CONFIG
#undef X
        // 提交
        cmd_list.UpdateBindlessArray(bindless_array);
    }

    void FreeFrameBuffers(BindlessArrayRef& bindless_array) {
        // 批量生成
#define X(TYPE, NAME, TEXTYPE, CONFIG) bindless_array->FreeTexture(NAME.handle);
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

#undef SCREEN_SIZE
#undef CUSTOMIZED_SIZE

#undef TexHandle
#undef E_SAMPLED_COLOR
#undef E_C_ATTACH
#undef E_D_S_ATTACH
#undef E_S_DEPTH

} // namespace Moer::Render::Raster