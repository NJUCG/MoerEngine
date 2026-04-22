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
    if (!ui_config.bloom_enabled) {
        return input_texture;
    }

    const uint mip_cnt = context.textures.bloom_downsample_chain.tex->GetNumMips();

    Sampler             linear_sampler{SF_LINEAR, SAM_CLAMP_TO_EDGE};
    BloomPrefilterParam prefilter_param;
    prefilter_param.threshold = 20.0f;
    prefilter_param.knee      = 0.5f;

    //Prefilter Pass
    context.cmd_list
        .Gfx(prefilter_pipeline, input_texture.tex->GetView(0, 1), linear_sampler, prefilter_param)
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
        BloomDownsampleParam downsample_param;

        uint2 src_size            = context.textures.bloom_downsample_chain.GetSize(i - 1);
        downsample_param.inv_size = float2(1.0f / src_size.x, 1.0f / src_size.y);

        context.cmd_list
            .Gfx(
                downsample_pipeline,
                context.textures.bloom_downsample_chain.tex->GetView(i - 1, 1),
                linear_sampler,
                downsample_param
            )
            .Draw(
                std::format("Downsample Pass #{}", i),
                context.textures.bloom_downsample_chain.GetRect2D(i),
                std::move(RasterTool::GetFullScreenDrawDatas()),
                ColorAttachment{context.textures.bloom_downsample_chain.tex, AC_LOAD_STORE, float4(0), i}
            );
    }

    // Upsample Pass
    for (int i = (int)mip_cnt - 2; i >= 0; --i) {
        BloomUpsampleParam upsample_param;

        uint2 small_size             = context.textures.bloom_downsample_chain.GetSize(i + 1);
        upsample_param.inv_size      = float2(1.0f / small_size.x, 1.0f / small_size.y);
        upsample_param.filter_radius = 1.2f;

        TextureView upsample_src = (i == (int)mip_cnt - 2) ?
                                       context.textures.bloom_downsample_chain.tex->GetView(i + 1, 1) :
                                       context.textures.bloom_upsample_chain.tex->GetView(i + 1, 1);

        context.cmd_list
            .Gfx(
                upsample_pipeline,
                upsample_src,
                context.textures.bloom_downsample_chain.tex->GetView(i, 1),
                linear_sampler,
                upsample_param
            )
            .Draw(
                std::format("Bloom Upsample Pass #{}", i),
                context.textures.bloom_upsample_chain.GetRect2D(i),
                std::move(RasterTool::GetFullScreenDrawDatas()),
                ColorAttachment{
                    context.textures.bloom_upsample_chain.tex, AC_CLEAR_STORE, float4(0), (uint32)i
                }
            );
    }

    BloomApplyParam apply_param;
    apply_param.bloom_intensity = 0.1f;

    context.cmd_list
        .Gfx(
            apply_pipeline,
            context.textures.bloom_upsample_chain.tex->GetView(0, 1),
            linear_sampler,
            apply_param
        )
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
