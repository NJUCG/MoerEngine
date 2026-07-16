#include "SkyboxPass.h"

// Implements skybox exposure correction and full-screen background rendering.
#include "RasterResource.h"
#include "RasterTextures.h"
#include "RasterTool.h"

namespace Moer::Render::Raster {
SkyboxPass::SkyboxPass(RasterContext& context) {
    RHIDepthStencilStateInfo depth_stencil_info(false, CO_GREATER_OR_EQUAL);

    GfxPsoCreateInfo pipeline_info(
        RHIRasterizeInfo::Preset(),
        {},
        {RHIColorAttachmentInfo::Preset(context.textures.lighting_output.tex->GetFormat())},
        depth_stencil_info,
        context.textures.depth_linear_sampler.tex->GetFormat()
    );

    skybox_pipeline = context.manager.Raster()
                          .Vertex("core/utils/FullScreenQuad.hlsl")
                          .Pixel("pipelines/raster/deferred/env_and_atmo/SkyboxPass.frag.hlsl")
                          .Build<SkyboxPipeline>(std::move(pipeline_info));
}

void SkyboxPass::Process(RasterContext& context, const RasterConfig& ui_config, const Camera& camera) {
    SkyboxPassBindlessParam skybox_param;
    skybox_param.cubemap_handle = context.textures.cubemap_tex.hdl;
    const auto& directional_light = context.GetSceneUpdates().main_directional_light;
    if (directional_light && ui_config.skybox_exposure_correct_enabled) {
        skybox_param.exposure_factor = directional_light->color * directional_light->intensity *
                                       powf(10.0f, ui_config.skybox_exposure_correct_factor_log10);
    } else {
        skybox_param.exposure_factor = float3(1.0f, 1.0f, 1.0f);
    }
    skybox_param.camera_pos = camera.GetPosition();
    skybox_param.clip2world = Transpose(camera.GetViewProjectionMatrixInv());

    DepthAttachment depth_attachment(
        context.textures.depth_linear_sampler.tex->GetView().GetTexture()
    );
    depth_attachment.action = EAttachmentAction::AC_DS_LOAD_STORE;

    context.cmd_list.Gfx(skybox_pipeline, context.bdls, skybox_param)
        .Draw(
            "Skybox Pass",
            context.textures.lighting_output.GetRect2D(),
            std::move(RasterTool::GetFullScreenDrawDatas()),
            depth_attachment,
            // Preserve lighting written by the previous deferred pass.
            ColorAttachment{
                context.textures.lighting_output.tex, EAttachmentAction::AC_LOAD_STORE, float4(0, 0, 0, 0)
            }
        );
}

} // namespace Moer::Render::Raster
