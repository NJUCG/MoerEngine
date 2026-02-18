#pragma once

#include "scene/Scene.h"

#include "RasterResource.h"
#include "RasterTextures.h"
#include "RasterTool.h"
#include "SkyboxPass.h"

namespace Moer::Render::Raster {
SkyboxPass::SkyboxPass(RasterContext& context) {
    RHIDepthStencilStateInfo ds_info(false, CO_GREATER_OR_EQUAL);

    GfxPsoCreateInfo pso_full_screen_info(
        RHIRasterizeInfo::Preset(),
        {},
        {RHIColorAttachmentInfo::Preset(context.textures.lighting_output.tex->GetFormat())},
        ds_info,
        context.textures.depth_linear_sampler.tex->GetFormat()
    );

    skybox_pipeline = context.manager.Raster()
                          .Vertex("core/utils/FullScreenQuad.hlsl")
                          .Pixel("pipelines/raster/deferred/env_and_atmo/SkyboxPass.frag.hlsl")
                          .Build<SkyboxPipeline>(std::move(pso_full_screen_info));
}

void SkyboxPass::Process(RasterContext& context, const RasterConfig& ui_config, const Camera& camera) {
    SkyboxPassBindlessParam skybox_param;
    skybox_param.cubemap_handle = context.textures.cubemap_tex.handle;
    auto directional_light_entt = context.scene.GetMainDirectionalLightEntity();
    if (directional_light_entt != entt::null && ui_config.skybox_exposure_correct_enabled) {
        auto directional_light       = context.scene.GetMainDirectionalLight();
        skybox_param.exposure_factor = directional_light.color * directional_light.intensity *
                                       powf(10.0f, ui_config.skybox_exposure_correct_factor_log10);
    } else {
        skybox_param.exposure_factor = float3(1.0f, 1.0f, 1.0f);
    }
    skybox_param.camera_pos    = camera.GetPosition();
    skybox_param.inv_view_proj = Transpose(camera.GetViewProjectionMatrixInv());

    DepthAttachment depth_att(context.textures.depth_linear_sampler.tex->GetView().GetTexture());
    depth_att.action = EAttachmentAction::AC_DS_LOAD_STORE;

    context.cmd_list.Gfx(skybox_pipeline, context.bdls, skybox_param)
        .Draw(
            "Skybox Pass",
            context.textures.lighting_output.GetRect2D(),
            std::move(RasterTool::GetFullScreenDrawDatas()),
            depth_att,
            ColorAttachment{
                context.textures.lighting_output.tex, EAttachmentAction::AC_LOAD_STORE, float4(0, 0, 0, 0)
            } //防止清空
        );
}

} // namespace Moer::Render::Raster