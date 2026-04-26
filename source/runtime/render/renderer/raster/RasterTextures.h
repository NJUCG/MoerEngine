#pragma once

#include "AssetTool.h"
#include "rhi/RHIResource.h"

namespace Moer::Render::Raster {

#define TexHandle       TextureWithHandle
#define E_SAMPLED_COLOR ETextureUsageFlags::SAMPLED | ETextureUsageFlags::COLOR_ATTACHMENT
#define E_C_ATTACH      ETextureUsageFlags::COLOR_ATTACHMENT
#define E_S_DEPTH       ETextureUsageFlags::SAMPLED | ETextureUsageFlags::DEPTH_STENCIL_ATTACHMENT
#define E_S_TRANSFER    ETextureUsageFlags::SAMPLED | ETextureUsageFlags::TRANSFER_DST

#define CUSTOMIZED_SIZE(x, y) Extent2D(x, y)

// Full-resolution only textures
#define RASTER_TEXTURES_TABLE                                                                                   \
    X(TexHandle, base_color, Tex2DTag, TexConfig::Default(PF_R8G8B8A8_UNORM).Usage(E_SAMPLED_COLOR))           \
    X(TexHandle, normal, Tex2DTag, TexConfig::Default(PF_A2R10G10B10_UNORM_PACK32).Usage(E_SAMPLED_COLOR))      \
    X(TexHandle, metal_rough_ao, Tex2DTag, TexConfig::Default(PF_R8G8B8A8_UNORM).Usage(E_SAMPLED_COLOR))       \
    X(TexHandle, shadow_mask, Tex2DTag, TexConfig::Default(PF_R8_UNORM).Usage(E_SAMPLED_COLOR))                 \
    X(TexHandle, lighting_output, Tex2DTag, TexConfig::Default(PF_R16G16B16A16_SFLOAT).Usage(E_SAMPLED_COLOR))  \
    X(TexHandle,                                                                                                \
      ao_output,                                                                                                \
      Tex2DTag,                                                                                                 \
      TexConfig::Default(PF_R16G16B16A16_SFLOAT)                                                                \
          .Usage(E_SAMPLED_COLOR | ETextureUsageFlags::UNORDERED_ACCESS))                                       \
    X(TexHandle, denoiser_output, Tex2DTag, TexConfig::Default(PF_R16G16B16A16_SFLOAT).Usage(E_SAMPLED_COLOR))  \
    X(TexHandle, upsample_output, Tex2DTag, TexConfig::Default(PF_R16G16B16A16_SFLOAT).Usage(E_SAMPLED_COLOR))  \
    X(TexHandle, ssr_output, Tex2DTag, TexConfig::Default(PF_R16G16B16A16_SFLOAT).Usage(E_SAMPLED_COLOR))       \
    X(TexHandle, aa_texture_1, Tex2DTag, TexConfig::Default(PF_R16G16B16A16_SFLOAT).Usage(E_SAMPLED_COLOR))     \
    X(TexHandle, aa_texture_2, Tex2DTag, TexConfig::Default(PF_R16G16B16A16_SFLOAT).Usage(E_SAMPLED_COLOR))     \
    X(TexHandle, aa_texture_3, Tex2DTag, TexConfig::Default(PF_R16G16B16A16_SFLOAT).Usage(E_SAMPLED_COLOR))     \
    X(TexHandle, aa_texture_4, Tex2DTag, TexConfig::Default(PF_R16G16B16A16_SFLOAT).Usage(E_SAMPLED_COLOR))     \
    X(TexHandle, aa_output, Tex2DTag, TexConfig::Default(PF_R16G16B16A16_SFLOAT).Usage(E_SAMPLED_COLOR))        \
    X(TexHandle,                                                                                                \
      bloom_downsample_chain,                                                                                   \
      Tex2DTag,                                                                                                 \
      TexConfig::Default(PF_B10G11R11_UFLOAT_PACK32)                                                            \
          .Usage(E_SAMPLED_COLOR)                                                                               \
          .Mips(6)                                                                                              \
          .IndivisualMips()                                                                                     \
          .SamplerConfig(SF_LINEAR, SAM_CLAMP_TO_EDGE))                                                         \
    X(TexHandle,                                                                                                \
      bloom_upsample_chain,                                                                                     \
      Tex2DTag,                                                                                                 \
      TexConfig::Default(PF_B10G11R11_UFLOAT_PACK32)                                                            \
          .Usage(E_SAMPLED_COLOR)                                                                               \
          .Mips(6)                                                                                              \
          .IndivisualMips()                                                                                     \
          .SamplerConfig(SF_LINEAR, SAM_CLAMP_TO_EDGE))                                                         \
    X(TexHandle, tonemapping_output, Tex2DTag, TexConfig::Default(PF_R8G8B8A8_UNORM).Usage(E_SAMPLED_COLOR))    \
    X(TexHandle, ui_frame_buffer, Tex2DTag, TexConfig::Default(PF_R8G8B8A8_UNORM).Usage(E_SAMPLED_COLOR))       \
    X(TexHandle, output, Tex2DTag, TexConfig::Default(PF_R8G8B8A8_SRGB).Usage(E_SAMPLED_COLOR))                 \
    X(DepthBufferWithHandle,                                                                                    \
      depth_linear_sampler,                                                                                     \
      TexDepthTag,                                                                                              \
        TexConfig::Default(PF_D32_SFLOAT)                                                                        \
          .Usage(E_S_DEPTH)                                                                                     \
          .SamplerConfig(SF_LINEAR, SAM_CLAMP_TO_EDGE))                                                         \
    X(DepthBufferWithHandle,                                                                                    \
      depth_nearest_sampler,                                                                                    \
      TexDepthTag,                                                                                              \
        TexConfig::Default(PF_D32_SFLOAT)                                                                        \
          .Usage(E_S_DEPTH)                                                                                     \
          .SamplerConfig(SF_NEAREST, SAM_CLAMP_TO_EDGE)                                                         \
          .From(depth_linear_sampler))                                                                          \
    X(TexHandle,                                                                                                \
      noise_tex,                                                                                                \
      Tex2DTag,                                                                                                 \
      TexConfig::Asset("noise_256x256.png")                                                                     \
          .Format(PF_R8G8B8A8_UNORM)                                                                            \
          .Usage(E_S_TRANSFER)                                                                                  \
          .SamplerConfig(SF_LINEAR, SAM_REPEAT))                                                                \
    X(TexHandle,                                                                                                \
      lut_ggx_emu,                                                                                              \
      Tex2DTag,                                                                                                 \
      TexConfig::Asset("LUT/GGX_E_LUT.png")                                                                     \
          .Format(PF_R8G8B8A8_UNORM)                                                                            \
          .Usage(E_S_TRANSFER)                                                                                  \
          .SamplerConfig(SF_LINEAR, SAM_REPEAT))                                                                \
    X(TexHandle,                                                                                                \
      lut_ggx_eavg,                                                                                             \
      Tex2DTag,                                                                                                 \
      TexConfig::Asset("LUT/GGX_Eavg_LUT.png")                                                                  \
          .Format(PF_R8G8B8A8_UNORM)                                                                            \
          .Usage(E_S_TRANSFER)                                                                                  \
          .SamplerConfig(SF_LINEAR, SAM_REPEAT))                                                                \
    X(TexHandle,                                                                                                \
      cubemap_tex,                                                                                              \
      TexCubeTag,                                                                                               \
      TexConfig::Asset("Skybox/WaterScene")                                                                     \
          .Format(PF_R8G8B8A8_UNORM)                                                                            \
          .Usage(E_S_TRANSFER)                                                                                  \
          .SamplerConfig(SF_LINEAR, SAM_REPEAT))

// Textures that also have a half-resolution (size/2) variant (NAME_half)
// Used for AO half-resolution mode
#define RASTER_TEXTURES_TABLE_DOWNSAMPLED                                                          \
    X(TexHandle,                                                                                   \
      ao_output_ambient_only,                                                                      \
      Tex2DTag,                                                                                    \
      TexConfig::Default(PF_R8_UNORM)                                                              \
          .Usage(E_SAMPLED_COLOR | ETextureUsageFlags::UNORDERED_ACCESS).DownSampled())            \
    X(TexHandle,                                                                                   \
      ao_output_ambient_only_1,                                                                    \
      Tex2DTag,                                                                                    \
      TexConfig::Default(PF_R8_UNORM)                                                              \
          .Usage(E_SAMPLED_COLOR | ETextureUsageFlags::UNORDERED_ACCESS).DownSampled())            \
    X(TexHandle,                                                                                   \
      ao_denoiser_accumulate,                                                                      \
      Tex2DTag,                                                                                    \
      TexConfig::Default(PF_R8_UNORM).Usage(E_SAMPLED_COLOR).DownSampled())                        \
    X(TexHandle,                                                                                   \
      ao_denoiser_accumulate_1,                                                                    \
      Tex2DTag,                                                                                    \
      TexConfig::Default(PF_R8_UNORM).Usage(E_SAMPLED_COLOR).DownSampled())                        \
    X(TexHandle,                                                                                   \
      camera_motion_vector,                                                                        \
      Tex2DTag,                                                                                    \
      TexConfig::Default(PF_R16G16_SFLOAT)                                                         \
          .Usage(E_SAMPLED_COLOR | ETextureUsageFlags::UNORDERED_ACCESS).DownSampled())

struct RasterTextures {
    // Full-resolution textures
#define X(TYPE, NAME, TEXTYPE, CONFIG) TYPE NAME;
    RASTER_TEXTURES_TABLE
    RASTER_TEXTURES_TABLE_DOWNSAMPLED
#undef X

    // Auto-generated half-resolution variants for downsampled textures
#define X(TYPE, NAME, TEXTYPE, CONFIG) TYPE NAME##_half;
    RASTER_TEXTURES_TABLE_DOWNSAMPLED
#undef X

    void CreateFrameBuffers(RenderDevice& device, const uint2& size) {
        // Full-resolution textures
#define X(TYPE, NAME, TEXTYPE, CONFIG)                                                  \
    {                                                                                   \
        TexConfig cfg = (CONFIG);                                                       \
        AssetTool::CreateRasterResource<TEXTYPE>(this->NAME, device, #NAME, size, cfg); \
    }
        RASTER_TEXTURES_TABLE
        RASTER_TEXTURES_TABLE_DOWNSAMPLED
#undef X

        // Half-resolution variants
        {
            uint2 half_size = uint2(std::max(1u, size.x / 2), std::max(1u, size.y / 2));
#define X(TYPE, NAME, TEXTYPE, CONFIG)                                                                    \
    {                                                                                                     \
        TexConfig cfg = (CONFIG);                                                                         \
        AssetTool::CreateRasterResource<TEXTYPE>(this->NAME##_half, device, #NAME "_half", half_size, cfg); \
    }
            RASTER_TEXTURES_TABLE_DOWNSAMPLED
#undef X
        }
    }

    void LoadAndUploadAssets(RenderDevice& device, CommandList& cmd_list) {
#define X(TYPE, NAME, TEXTYPE, CONFIG)                                               \
    {                                                                                \
        TexConfig cfg = (CONFIG);                                                    \
        if (cfg.is_asset) {                                                          \
            AssetTool::LoadTexture<TEXTYPE>(device, cmd_list, NAME.tex, cfg, #NAME); \
        }                                                                            \
    }
        RASTER_TEXTURES_TABLE
        RASTER_TEXTURES_TABLE_DOWNSAMPLED
#undef X
    }

    void AllocateFrameBuffers(CommandList& cmd_list, BindlessArrayRef& bindless_array) {
        // Full-resolution textures
#define X(TYPE, NAME, TEXTYPE, CONFIG) \
    AssetTool::AllocateRasterResourceHandle(bindless_array, NAME, (CONFIG));
        RASTER_TEXTURES_TABLE
        RASTER_TEXTURES_TABLE_DOWNSAMPLED
#undef X

        // Half-resolution variants (LINEAR sampler for bilinear upsampling)
#define X(TYPE, NAME, TEXTYPE, CONFIG)                                                         \
    {                                                                                          \
        TexConfig half_cfg;                                                                    \
        half_cfg.sampler = {ESamplerFilter::SF_LINEAR, ESamplerAddressMode::SAM_CLAMP_TO_EDGE};\
        AssetTool::AllocateRasterResourceHandle(bindless_array, NAME##_half, half_cfg);        \
    }
        RASTER_TEXTURES_TABLE_DOWNSAMPLED
#undef X

        cmd_list.UpdateBindlessArray(bindless_array);
    }

    void FreeFrameBuffers(BindlessArrayRef& bindless_array, bool is_free_external_assets) {
#define X(TYPE, NAME, TEXTYPE, CONFIG)                                 \
    {                                                                  \
        TexConfig cfg = (CONFIG);                                      \
        if (!cfg.is_asset || is_free_external_assets) {                \
            AssetTool::FreeRasterResourceHandle(bindless_array, NAME); \
        }                                                              \
    }
        RASTER_TEXTURES_TABLE
        RASTER_TEXTURES_TABLE_DOWNSAMPLED
#undef X

        // Half-resolution variants
#define X(TYPE, NAME, TEXTYPE, CONFIG) \
    AssetTool::FreeRasterResourceHandle(bindless_array, NAME##_half);
        RASTER_TEXTURES_TABLE_DOWNSAMPLED
#undef X
    }

    Array<TextureView> GetDisplayableFrameBuffersView() {
        Array<TextureView> views;
#define X(TYPE, NAME, TEXTYPE, CONFIG)                                     \
    assert(this->NAME.tex != nullptr && "There is an empty FrameBuffer!"); \
    views.emplace_back(this->NAME.tex->GetView());
        RASTER_TEXTURES_TABLE
        RASTER_TEXTURES_TABLE_DOWNSAMPLED
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
        return views;
    }
};

#undef RASTER_TEXTURES_TABLE
#undef RASTER_TEXTURES_TABLE_DOWNSAMPLED

#undef CUSTOMIZED_SIZE

#undef TexHandle
#undef E_SAMPLED_COLOR
#undef E_C_ATTACH
#undef E_S_DEPTH
#undef E_S_TRANSFER

} // namespace Moer::Render::Raster
