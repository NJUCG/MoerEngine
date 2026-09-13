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

BloomPass::RecordParameters BloomPass::Prepare(
    const RasterContext& context,
    const RasterConfig&  raster_config,
    TextureWithHandle    input_texture
) const {
    RecordParameters parameters{};
    parameters.enabled          = raster_config.bloom_enabled;
    parameters.input            = std::move(input_texture);
    parameters.downsample_chain = context.textures.bloom_downsample_chain;
    parameters.upsample_chain   = context.textures.bloom_upsample_chain;
    parameters.prefilter.threshold = 20.0f;
    parameters.prefilter.knee      = 0.5f;
    parameters.apply.bloom_intensity = 0.1f;
    if (!parameters.enabled) {
        return parameters;
    }

    const uint mip_count = parameters.downsample_chain.tex->GetNumMips();
    parameters.downsample_dispatches.reserve(mip_count > 0 ? mip_count - 1 : 0);
    for (uint mip = 1; mip < mip_count; ++mip) {
        DownsampleDispatch dispatch{};
        const uint2 source_size = parameters.downsample_chain.GetSize(mip - 1);
        dispatch.source          = parameters.downsample_chain.tex->GetView(mip - 1, 1);
        dispatch.render_area     = parameters.downsample_chain.GetRect2D(mip);
        dispatch.shader.inv_size = float2(1.0f / source_size.x, 1.0f / source_size.y);
        dispatch.mip             = mip;
        parameters.downsample_dispatches.emplace_back(std::move(dispatch));
    }

    parameters.upsample_dispatches.reserve(mip_count > 0 ? mip_count - 1 : 0);
    for (int mip = static_cast<int>(mip_count) - 2; mip >= 0; --mip) {
        UpsampleDispatch dispatch{};
        const uint2 small_size = parameters.downsample_chain.GetSize(mip + 1);
        dispatch.source = mip == static_cast<int>(mip_count) - 2 ?
                              parameters.downsample_chain.tex->GetView(mip + 1, 1) :
                              parameters.upsample_chain.tex->GetView(mip + 1, 1);
        dispatch.downsample = parameters.downsample_chain.tex->GetView(mip, 1);
        dispatch.render_area = parameters.upsample_chain.GetRect2D(mip);
        dispatch.shader.inv_size = float2(1.0f / small_size.x, 1.0f / small_size.y);
        dispatch.shader.filter_radius = 1.2f;
        dispatch.mip = static_cast<uint32>(mip);
        parameters.upsample_dispatches.emplace_back(std::move(dispatch));
    }
    return parameters;
}

void BloomPass::Record(CommandList& cmd_list, const RecordParameters& parameters) {
    if (!parameters.enabled) {
        return;
    }

    cmd_list
        .Gfx(
            prefilter_pipeline,
            parameters.input.tex->GetView(0, 1),
            parameters.linear_sampler,
            parameters.prefilter
        )
        .Draw(
            "Bloom Prefilter Pass",
            parameters.downsample_chain.GetRect2D(0),
            std::move(RasterTool::GetFullScreenDrawDatas()),
            ColorAttachment{
                parameters.downsample_chain.tex,
                EAttachmentAction::AC_CLEAR_STORE,
                float4(0, 0, 0, 0),
                0
            }
        );

    for (const DownsampleDispatch& dispatch : parameters.downsample_dispatches) {
        cmd_list
            .Gfx(
                downsample_pipeline,
                dispatch.source,
                parameters.linear_sampler,
                dispatch.shader
            )
            .Draw(
                std::format("Downsample Pass #{}", dispatch.mip),
                dispatch.render_area,
                std::move(RasterTool::GetFullScreenDrawDatas()),
                ColorAttachment{
                    parameters.downsample_chain.tex,
                    AC_CLEAR_STORE,
                    float4(0),
                    dispatch.mip
                }
            );
    }

    for (const UpsampleDispatch& dispatch : parameters.upsample_dispatches) {
        cmd_list
            .Gfx(
                upsample_pipeline,
                dispatch.source,
                dispatch.downsample,
                parameters.linear_sampler,
                dispatch.shader
            )
            .Draw(
                std::format("Bloom Upsample Pass #{}", dispatch.mip),
                dispatch.render_area,
                std::move(RasterTool::GetFullScreenDrawDatas()),
                ColorAttachment{
                    parameters.upsample_chain.tex,
                    AC_CLEAR_STORE,
                    float4(0),
                    dispatch.mip
                }
            );
    }

    cmd_list
        .Gfx(
            apply_pipeline,
            parameters.upsample_chain.tex->GetView(0, 1),
            parameters.linear_sampler,
            parameters.apply
        )
        .Draw(
            "Apply Bloom to Scene",
            parameters.input.GetRect2D(),
            std::move(RasterTool::GetFullScreenDrawDatas()),
            ColorAttachment{
                parameters.input.tex,
                EAttachmentAction::AC_LOAD_STORE,
                float4(0, 0, 0, 0),
                0
            }
        );
}

TextureWithHandle BloomPass::Process(
    RasterContext& context, const RasterConfig& raster_config, TextureWithHandle input_texture
) {
    const RecordParameters parameters = Prepare(context, raster_config, std::move(input_texture));
    Record(context.cmd_list, parameters);
    return parameters.input;
}

} // namespace Moer::Render::Raster
