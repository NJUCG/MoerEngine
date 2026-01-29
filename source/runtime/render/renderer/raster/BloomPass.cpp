#pragma once

#include "scene/Camera.h"
#include "scene/Material.h"
#include "scene/RenderableManager.h"
#include "scene/light/LightComponentManager.h"

#include "BloomPass.h"
#include "RasterResource.h"
#include "RasterTextures.h"
#include "RasterTool.h"

namespace Moer::Render::Raster {
BloomPass::BloomPass(RasterContext& context) {
    RHIRasterizeInfo rast_info    = RHIRasterizeInfo::Preset();
    EPixelFormat     bloom_format = PF_B10G11R11_UFLOAT_PACK32;
    EPixelFormat     scene_format = context.textures.lighting_output.tex->GetFormat();

    GfxPsoCreateInfo prefilter_info(
        rast_info, {}, {RHIColorAttachmentInfo::Preset<Moer::Render::Blend::NONE>(bloom_format)}
    );
    prefilter_pipeline = context.manager.Raster()
                             .Vertex("core/utils/FullScreenQuad.hlsl")
                             .Pixel("pipelines/postprocess/color/BloomPrefilter.frag.hlsl")
                             .Build<BloomPassPrefilterPipeline>(std::move(prefilter_info));

    GfxPsoCreateInfo downsample_info(
        rast_info, {}, {RHIColorAttachmentInfo::Preset<Moer::Render::Blend::NONE>(bloom_format)}
    );
    downsample_pipeline = context.manager.Raster()
                              .Vertex("core/utils/FullScreenQuad.hlsl")
                              .Pixel("pipelines/postprocess/color/BloomDownSample.frag.hlsl")
                              .Build<BloomPassDownSamplePipeline>(std::move(downsample_info));

    GfxPsoCreateInfo upsample_info(
        rast_info, {}, {RHIColorAttachmentInfo::Preset<Moer::Render::Blend::NONE>(bloom_format)}
    );
    upsample_pipeline = context.manager.Raster()
                            .Vertex("core/utils/FullScreenQuad.hlsl")
                            .Pixel("pipelines/postprocess/color/BloomUpSample.frag.hlsl")
                            .Build<BloomPassUpSamplePipeline>(std::move(upsample_info));

    GfxPsoCreateInfo apply_info(
        rast_info, {}, {RHIColorAttachmentInfo::Preset<Moer::Render::Blend::ADDITIVE_BLEND>(scene_format)}
    );
    apply_pipeline = context.manager.Raster()
                         .Vertex("core/utils/FullScreenQuad.hlsl")
                         .Pixel("pipelines/postprocess/color/BloomApply.frag.hlsl")
                         .Build<BloomApplyPipeline>(std::move(apply_info));
}

TextureWithHandle
BloomPass::Process(RasterContext& context, const RasterConfig& ui_config, TextureWithHandle& input_texture) {
    const uint mip_cnt = context.textures.bloom_downsample_chain.tex->GetNumMips();

    BloomPrefilterBindlessParam prefilter_param;
    prefilter_param.input_tex_hdl = input_texture.handle;
    prefilter_param.threshold     = 20.0f;
    prefilter_param.knee          = 0.5f;

    context.cmd_list.TextureBarriers(
        EQueueType::Graphics,
        EQueueType::Graphics,
        EPassType::Graphics,
        Array<ReadTexture>{{input_texture.tex->GetView(0, 1), ETextureState::SAMPLE}},
        Array<WriteTexture>{
            {context.textures.bloom_downsample_chain.tex->GetView(0, 1), ETextureState::RENDER_TARGET}
        }
    );

    //Prefilter Pass
    context.cmd_list.Gfx(prefilter_pipeline, context.bdls, prefilter_param)
        .Draw(
            "Bloom Prefilter Pass",
            context.textures.bloom_downsample_chain.GetRect2D(0),
            std::move(RasterTool::GetFullScreenDrawDatas()),
            ColorAttachment{
                context.textures.bloom_downsample_chain.tex,
                EAttachmentAction::AC_CLEAR_STORE,
                float4(0, 0, 0, 0),
                0 // mip_level
            }
        );

    // Downsample Pass
    for (uint i = 1; i < mip_cnt; ++i) {
        BloomDownsampleBindlessParam downsample_param;
        downsample_param.downsample_chain_hdl = context.textures.bloom_downsample_chain.mip_handles[i - 1];

        uint2 src_size            = context.textures.bloom_downsample_chain.GetSize(i - 1);
        downsample_param.inv_size = float2(1.0f / src_size.x, 1.0f / src_size.y);

        context.cmd_list.TextureBarriers(
            EQueueType::Graphics,
            EQueueType::Graphics,
            EPassType::Graphics,
            Array<ReadTexture>{
                {context.textures.bloom_downsample_chain.tex->GetView(i - 1, 1), ETextureState::SAMPLE}
            },
            Array<WriteTexture>{
                {context.textures.bloom_downsample_chain.tex->GetView(i, 1), ETextureState::RENDER_TARGET}
            }
        );

        context.cmd_list.Gfx(downsample_pipeline, context.bdls, downsample_param)
            .Draw(
                std::format("Downsample Pass #{}", i),
                context.textures.bloom_downsample_chain.GetRect2D(i),
                std::move(RasterTool::GetFullScreenDrawDatas()),
                ColorAttachment{context.textures.bloom_downsample_chain.tex, AC_LOAD_STORE, float4(0), i}
            );
    }

    // Upsample Pass
    for (int i = (int)mip_cnt - 2; i >= 0; --i) {
        BloomUpsampleBindlessParam upsample_param;
        // 当前层细节 (Down 链)
        upsample_param.downsample_chain_hdl = context.textures.bloom_downsample_chain.mip_handles[i];

        // 下一层模糊结果 (Up 链 或 Down 链底层)
        if (i == (int)mip_cnt - 2) {
            upsample_param.upsample_chain_hdl = context.textures.bloom_downsample_chain.mip_handles[i + 1];
        } else {
            upsample_param.upsample_chain_hdl = context.textures.bloom_upsample_chain.mip_handles[i + 1];
        }

        uint2 small_size             = context.textures.bloom_downsample_chain.GetSize(i + 1);
        upsample_param.inv_size      = float2(1.0f / small_size.x, 1.0f / small_size.y);
        upsample_param.filter_radius = 1.2f;

        context.cmd_list.TextureBarriers(
            EQueueType::Graphics,
            EQueueType::Graphics,
            EPassType::Graphics,
            Array<ReadTexture>{
                {context.textures.bloom_downsample_chain.tex->GetView(i, 1), ETextureState::SAMPLE},
                {context.textures.bloom_upsample_chain.tex->GetView(i + 1, 1), ETextureState::SAMPLE}
            },
            Array<WriteTexture>{
                {context.textures.bloom_upsample_chain.tex->GetView(i, 1), ETextureState::RENDER_TARGET}
            }
        );

        context.cmd_list.Gfx(upsample_pipeline, context.bdls, upsample_param)
            .Draw(
                std::format("Bloom Upsample Pass #{}", i),
                context.textures.bloom_upsample_chain.GetRect2D(i),
                std::move(RasterTool::GetFullScreenDrawDatas()),
                ColorAttachment{
                    context.textures.bloom_upsample_chain.tex, AC_CLEAR_STORE, float4(0), (uint32)i
                }
            );
    }

    BloomApplyBindlessParam apply_param;
    // 最终结果在 upsample 链的 mip 0
    apply_param.bloom_result_hdl = context.textures.bloom_upsample_chain.mip_handles[0];
    apply_param.bloom_intensity  = 0.1f;

    context.cmd_list.TextureBarriers(
        EQueueType::Graphics,
        EQueueType::Graphics,
        EPassType::Graphics,
        Array<ReadTexture>{{context.textures.bloom_upsample_chain.tex->GetView(0, 1), ETextureState::SAMPLE}},
        Array<WriteTexture>{{input_texture.tex->GetView(0, 1), ETextureState::RENDER_TARGET}}
    );

    context.cmd_list.Gfx(apply_pipeline, context.bdls, apply_param)
        .Draw(
            "Apply Bloom to Scene",
            input_texture.GetRect2D(),
            std::move(RasterTool::GetFullScreenDrawDatas()),
            ColorAttachment{
                input_texture.tex,
                EAttachmentAction::AC_LOAD_STORE,
                float4(0, 0, 0, 0),
                0 // mip_level
            }
        );

    return input_texture;
}
} // namespace Moer::Render::Raster
