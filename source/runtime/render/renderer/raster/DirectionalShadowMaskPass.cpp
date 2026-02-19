#include "DirectionalShadowMaskPass.h"

#include "RasterTool.h"

namespace Moer::Render::Raster {
DirectionalShadowMaskPass::DirectionalShadowMaskPass(RasterContext& context) {
    GfxPsoCreateInfo pso_full_screen_info(
        RHIRasterizeInfo::Preset(),
        {},
        {RHIColorAttachmentInfo::Preset(context.textures.shadow_mask.tex->GetFormat())}
    );

    directional_shadow_mask_pipeline =
        context.manager.Raster()
            .Vertex("core/utils/FullScreenQuad.hlsl")
            .Pixel("pipelines/raster/deferred/lighting/shadows/ShadowMask.frag.hlsl")
            .Build<DirectionalShadowMaskPassPipeline>(std::move(pso_full_screen_info));
}

void DirectionalShadowMaskPass::Process(
    RasterContext&      context,
    const RasterConfig& ui_config,
    const Camera&       camera
) {
    DirectionalShadowMaskPassBindlessParam param;
    param.global_param_hdl = context.lighting_data_buffer.hdl;
    param.normal_hdl       = context.textures.normal.hdl;
    param.depth_hdl        = context.textures.depth_nearest_sampler.hdl;

    context.cmd_list.Gfx(directional_shadow_mask_pipeline, context.bdls, param)
        .Draw(
            "Directional Shadow Mask Pass",
            context.textures.shadow_mask.GetRect2D(),
            std::move(RasterTool::GetFullScreenDrawDatas()),
            ColorAttachment(context.textures.shadow_mask.tex)
        );
}
} // namespace Moer::Render::Raster