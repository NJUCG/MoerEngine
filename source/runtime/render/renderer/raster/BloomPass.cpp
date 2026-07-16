#include "BloomPass.h"

// 实现 Bloom 的预过滤、下采样、上采样和加法合成阶段。
#include "RasterResource.h"
#include "RasterTextures.h"
#include "RasterTool.h"

namespace Moer::Render::Raster {
BloomPass::BloomPass(RasterContext& context) {
    const RHIRasterizeInfo rasterize_info = RHIRasterizeInfo::Preset();
    const EPixelFormat     bloom_format   = PF_B10G11R11_UFLOAT_PACK32;
    const EPixelFormat     scene_format   = context.textures.lighting_output.tex->GetFormat();

    GfxPsoCreateInfo prefilter_info(
        rasterize_info, {}, {RHIColorAttachmentInfo::Preset<Moer::Render::Blend::NONE>(bloom_format)}
    );
    prefilter_pipeline = context.manager.Raster()
                             .Vertex("core/utils/FullScreenQuad.hlsl")
                             .Pixel("pipelines/postprocess/color/BloomPrefilter.frag.hlsl")
                             .Build<BloomPassPrefilterPipeline>(std::move(prefilter_info));

    GfxPsoCreateInfo downsample_info(
        rasterize_info, {}, {RHIColorAttachmentInfo::Preset<Moer::Render::Blend::NONE>(bloom_format)}
    );
    downsample_pipeline = context.manager.Raster()
                              .Vertex("core/utils/FullScreenQuad.hlsl")
                              .Pixel("pipelines/postprocess/color/BloomDownSample.frag.hlsl")
                              .Build<BloomPassDownSamplePipeline>(std::move(downsample_info));

    GfxPsoCreateInfo upsample_info(
        rasterize_info, {}, {RHIColorAttachmentInfo::Preset<Moer::Render::Blend::NONE>(bloom_format)}
    );
    upsample_pipeline = context.manager.Raster()
                            .Vertex("core/utils/FullScreenQuad.hlsl")
                            .Pixel("pipelines/postprocess/color/BloomUpSample.frag.hlsl")
                            .Build<BloomPassUpSamplePipeline>(std::move(upsample_info));

    GfxPsoCreateInfo apply_info(
        rasterize_info,
        {},
        {RHIColorAttachmentInfo::Preset<Moer::Render::Blend::ADDITIVE_BLEND>(scene_format)}
    );
    apply_pipeline = context.manager.Raster()
                         .Vertex("core/utils/FullScreenQuad.hlsl")
                         .Pixel("pipelines/postprocess/color/BloomApply.frag.hlsl")
                         .Build<BloomApplyPipeline>(std::move(apply_info));
}

TextureWithHandle
BloomPass::Process(
    RasterContext&      context,
    const RasterConfig& raster_config,
    TextureWithHandle&  input_texture
) {
    if (!raster_config.bloom_enabled) {
        return input_texture;
    }

    const uint mip_count = context.textures.bloom_downsample_chain.tex->GetNumMips();

    Sampler             linear_sampler{SF_LINEAR, SAM_CLAMP_TO_EDGE};
    BloomPrefilterParam prefilter_param;
    prefilter_param.threshold = 20.0f;
    prefilter_param.knee      = 0.5f;

    // 构建低分辨率金字塔前，先将高亮区域提取到 mip 0。
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

    // 下采样阶段。
    for (uint mip_index = 1; mip_index < mip_count; ++mip_index) {
        BloomDownsampleParam downsample_param;

        const uint2 source_size =
            context.textures.bloom_downsample_chain.GetSize(mip_index - 1);
        downsample_param.inv_size = float2(1.0f / source_size.x, 1.0f / source_size.y);

        context.cmd_list
            .Gfx(
                downsample_pipeline,
                context.textures.bloom_downsample_chain.tex->GetView(mip_index - 1, 1),
                linear_sampler,
                downsample_param
            )
            .Draw(
                std::format("Downsample Pass #{}", mip_index),
                context.textures.bloom_downsample_chain.GetRect2D(mip_index),
                std::move(RasterTool::GetFullScreenDrawDatas()),
                ColorAttachment{
                    context.textures.bloom_downsample_chain.tex,
                    AC_LOAD_STORE,
                    float4(0),
                    mip_index
                }
            );
    }

    // 上采样阶段。使用独立纹理链，避免采样当前正在写入的 mip。
    for (int mip_index = static_cast<int>(mip_count) - 2; mip_index >= 0; --mip_index) {
        BloomUpsampleParam upsample_param;

        const uint2 small_size = context.textures.bloom_downsample_chain.GetSize(mip_index + 1);
        upsample_param.inv_size      = float2(1.0f / small_size.x, 1.0f / small_size.y);
        upsample_param.filter_radius = 1.2f;

        const TextureView upsample_source =
            mip_index == static_cast<int>(mip_count) - 2 ?
                context.textures.bloom_downsample_chain.tex->GetView(mip_index + 1, 1) :
                context.textures.bloom_upsample_chain.tex->GetView(mip_index + 1, 1);

        context.cmd_list
            .Gfx(
                upsample_pipeline,
                upsample_source,
                context.textures.bloom_downsample_chain.tex->GetView(mip_index, 1),
                linear_sampler,
                upsample_param
            )
            .Draw(
                std::format("Bloom Upsample Pass #{}", mip_index),
                context.textures.bloom_upsample_chain.GetRect2D(mip_index),
                std::move(RasterTool::GetFullScreenDrawDatas()),
                ColorAttachment{
                    context.textures.bloom_upsample_chain.tex,
                    AC_CLEAR_STORE,
                    float4(0),
                    static_cast<uint32>(mip_index)
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
