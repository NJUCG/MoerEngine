#pragma once

#include "rhi/RHI.h"
#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/raster/post_process/ShaderParameters.h"

namespace Moer::Render::Raster {

struct TextureWithHandle {
    TextureRef tex;
    uint       handle;
};
struct DepthBufferWithHandle {
    DepthBufferRef tex;
    uint           handle;
};
struct BufferWithHandle {
    BufferRef buf;
    uint      handle;
};

// 如果texture的名字不是编译期决定的，则需要找一个地方存名字。否则string_view会出现悬垂指针
struct DepthBufferWithHandleAndName {
    DepthBufferRef tex;
    uint           handle;
    std::string    name;
};

/**
 * 下面使用宏来维护Raster中所需要的Textures，避免代码过多导致的维护困难（或许，如果某天发现这么写还是难以使用，可以改回去）
 * 
 * 例外：
 *   - depth手动维护，原因是需要两个不同的Sampler (nearest, linear)
 */

#define E_SAMPLED      ETextureUsageFlags::SAMPLED
#define E_COLOR_ATTACH ETextureUsageFlags::COLOR_ATTACHMENT
#define E_D_S_ATTACH   ETextureUsageFlags::DEPTH_STENCIL_ATTACHMENT

#define SCREEN_SIZE           Extent2D(size.x, size.y)
#define CUSTOMIZED_SIZE(x, y) Extent2D(x, y)

#define RASTER_TEXTURES_TABLE                                                                           \
    X(TextureWithHandle, vbuffer, PF_R32_UINT, E_SAMPLED | E_COLOR_ATTACH, SCREEN_SIZE)                 \
    X(TextureWithHandle, normal, PF_A2R10G10B10_UNORM_PACK32, E_SAMPLED | E_COLOR_ATTACH, SCREEN_SIZE)  \
    X(TextureWithHandle, tangent, PF_A2R10G10B10_UNORM_PACK32, E_SAMPLED | E_COLOR_ATTACH, SCREEN_SIZE) \
    X(TextureWithHandle, uv, PF_R32G32_SFLOAT, E_SAMPLED | E_COLOR_ATTACH, SCREEN_SIZE)                 \
    X(TextureWithHandle, position, PF_R32G32B32A32_SFLOAT, E_SAMPLED | E_COLOR_ATTACH, SCREEN_SIZE)     \
    X(TextureWithHandle, lighting_output, PF_R8G8B8A8_UNORM, E_SAMPLED | E_COLOR_ATTACH, SCREEN_SIZE)   \
    X(TextureWithHandle, ao_output, PF_R8G8B8A8_UNORM, E_SAMPLED | E_COLOR_ATTACH, SCREEN_SIZE)         \
    X(TextureWithHandle, ssr_output, PF_R8G8B8A8_UNORM, E_SAMPLED | E_COLOR_ATTACH, SCREEN_SIZE)        \
    X(TextureWithHandle, aa_texture_1, PF_R8G8B8A8_UNORM, E_SAMPLED | E_COLOR_ATTACH, SCREEN_SIZE)      \
    X(TextureWithHandle, aa_texture_2, PF_R8G8B8A8_UNORM, E_SAMPLED | E_COLOR_ATTACH, SCREEN_SIZE)      \
    X(TextureWithHandle, aa_texture_3, PF_R8G8B8A8_UNORM, E_SAMPLED | E_COLOR_ATTACH, SCREEN_SIZE)      \
    X(TextureWithHandle, aa_texture_4, PF_R8G8B8A8_UNORM, E_SAMPLED | E_COLOR_ATTACH, SCREEN_SIZE)      \
    X(TextureWithHandle, aa_output, PF_R8G8B8A8_UNORM, E_SAMPLED | E_COLOR_ATTACH, SCREEN_SIZE)         \
    X(TextureWithHandle, ui_frame_buffer, PF_R8G8B8A8_SRGB, E_SAMPLED | E_COLOR_ATTACH, SCREEN_SIZE)    \
    X(TextureWithHandle, output, PF_R8G8B8A8_SRGB, E_COLOR_ATTACH, SCREEN_SIZE)

struct RasterTextures {
    // 批量生成
#define X(TYPE, NAME, PF, USAGE, SIZE) TYPE NAME;
    RASTER_TEXTURES_TABLE
#undef X
    // 手动维护: depth
    DepthBufferWithHandle depth_linear_sampler;
    DepthBufferWithHandle depth_nearest_sampler;

    void CreateFrameBuffers(RenderDevice& device, uint2 size) {
        // 批量生成
#define X(TYPE, NAME, PF, USAGE, SIZE) \
    NAME.tex = device.CreateTexture(#NAME, Extent2D(size.x, size.y), PF, USAGE);
        RASTER_TEXTURES_TABLE
#undef X
        // 手动维护: depth
        depth_linear_sampler.tex = device.CreateDepthBuffer(
            "depth",
            Extent2D(size.x, size.y),
            PF_D32_SFLOAT_S8_UINT,
            1,
            ETextureUsageFlags::SAMPLED | ETextureUsageFlags::DEPTH_STENCIL_ATTACHMENT
        );
        depth_nearest_sampler.tex = depth_linear_sampler.tex;
    }

    void AllocateFrameBuffers(CommandList& cmd_list, BindlessArrayRef& bindless_array) {
        // 默认Sampler
        Sampler linear_sampler(SF_LINEAR, SAM_REPEAT);

        // 批量生成
#define X(TYPE, NAME, PF, USAGE, SIZE) \
    NAME.handle = bindless_array->AllocateTexture(NAME.tex, linear_sampler);
        RASTER_TEXTURES_TABLE
#undef X
        // 手动维护: depth
        depth_linear_sampler.handle = bindless_array->AllocateTexture(
            depth_linear_sampler.tex->GetView(), Sampler(SF_NEAREST, SAM_CLAMP_TO_EDGE)
        );
        depth_nearest_sampler.handle = bindless_array->AllocateTexture(
            depth_nearest_sampler.tex->GetView(), Sampler(SF_LINEAR, SAM_CLAMP_TO_EDGE)
        );

        // 提交
        cmd_list.UpdateBindlessArray(bindless_array);
    }

    void FreeFrameBuffers(BindlessArrayRef& bindless_array) {
        // 批量生成
#define X(TYPE, NAME, PF, USAGE, SIZE) bindless_array->FreeTexture(NAME.handle);
        RASTER_TEXTURES_TABLE
#undef X
        // 手动维护: depth
        bindless_array->FreeTexture(depth_linear_sampler.handle);
        bindless_array->FreeTexture(depth_nearest_sampler.handle);
    }

    Array<TextureView> GetDisplayableFrameBuffersView() {
        Array<TextureView> views;
        // 手动维护: depth (这里把depth push在前面，这样GUI里就会显示在最前面)
        views.emplace_back(depth_linear_sampler.tex->GetView());
        // 批量生成
#define X(TYPE, NAME, PF, USAGE, SIZE)                               \
    assert(NAME.tex != nullptr && "There is an empty FrameBuffer!"); \
    views.emplace_back(NAME.tex->GetView());
        RASTER_TEXTURES_TABLE
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
};

#undef RASTER_TEXTURES_TABLE

#undef SCREEN_SIZE
#undef CUSTOMIZED_SIZE

#undef E_SAMPLED
#undef E_COLOR_ATTACH
#undef E_D_S_ATTACH

} // namespace Moer::Render::Raster