// 实现全屏方向光阴影遮罩 Pass。
#include "DirectionalShadowMaskPass.h"

#include "RasterTool.h"

namespace Moer::Render::Raster {
DirectionalShadowMaskPass::DirectionalShadowMaskPass(RasterContext& context) {
    GfxPsoCreateInfo pso_full_screen_info(
        RHIRasterizeInfo::Preset(),
        {},
        {RHIColorAttachmentInfo::Preset(context.textures.shadow_mask.tex->GetFormat())}
    );

    pipeline = context.manager.Raster()
                   .Vertex("core/utils/FullScreenQuad.hlsl")
                   .Pixel("pipelines/raster/deferred/lighting/shadows/ShadowMask.frag.hlsl")
                   .Build<DirectionalShadowMaskPassPipeline>(std::move(pso_full_screen_info));
}

DirectionalShadowMaskPass::RecordParameters
DirectionalShadowMaskPass::Prepare(const RasterContext& context) const {
    RecordParameters parameters{
        .normal_handle = context.textures.normal.hdl,
        .depth_handle  = context.textures.depth_nearest_sampler.hdl,
        .normal_owner  = context.textures.normal.tex,
        .depth_owner   = context.textures.depth_nearest_sampler.tex,
        .lighting_data = context.lighting_data_buffer.buf,
        .bindless      = context.bdls,
        .output        = context.textures.shadow_mask.tex,
        .render_area   = context.textures.shadow_mask.GetRect2D(),
    };
    for (size_t index = 0; index < parameters.cascade_shadow_owners.size(); ++index) {
        parameters.cascade_shadow_owners[index] = context.csm_data.shadow_map_textures[index].tex;
    }
    for (size_t index = 0; index < parameters.point_shadow_owners.size(); ++index) {
        parameters.point_shadow_owners[index] = context.point_shadow_data.shadow_cubes[index].tex;
    }
    return parameters;
}

void DirectionalShadowMaskPass::Record(
    CommandList&            cmd_list,
    const RecordParameters& parameters
) {
    DirectionalShadowMaskPassBindlessParam param;
    param.normal_hdl = parameters.normal_handle;
    param.depth_hdl  = parameters.depth_handle;

    cmd_list
        .Gfx(pipeline, parameters.lighting_data, parameters.bindless, param)
        .Draw(
            "Directional Shadow Mask Pass",
            parameters.render_area,
            std::move(RasterTool::GetFullScreenDrawDatas()),
            ColorAttachment(parameters.output)
        );
}

void DirectionalShadowMaskPass::Process(RasterContext& context) {
    Record(context.cmd_list, Prepare(context));
}
} // namespace Moer::Render::Raster
