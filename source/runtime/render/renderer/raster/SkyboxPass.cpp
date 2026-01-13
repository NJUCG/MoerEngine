#pragma once

#include "scene/Camera.h"
#include "scene/Material.h"
#include "scene/RenderableManager.h"
#include "scene/light/LightComponentManager.h"

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

void SkyboxPass::Process(RasterContext& context, const RasterConfig& ui_config, const CameraRef& camera) {
    SkyboxPassBindlessParam skybox_param;
    skybox_param.cubemap_handle = context.textures.cubemap_tex.handle;
    auto directional_light      = GetMainLightDirection(context);
    if (directional_light != nullptr && ui_config.skybox_exposure_correct_enabled) {
        skybox_param.exposure_factor = directional_light->GetColor() * directional_light->GetIntensity() *
                                       powf(10.0f, ui_config.skybox_exposure_correct_factor_log10);
    } else {
        skybox_param.exposure_factor = float3(1.0f, 1.0f, 1.0f);
    }
    skybox_param.camera_pos    = camera->GetPosition();
    skybox_param.inv_view_proj = Transpose(camera->GetViewProjectionMatrixInv());

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

DirectionalLightComponent* SkyboxPass::GetMainLightDirection(RasterContext& context) {
    auto lights          = context.scene.GetLights();
    auto light_component = LightComponentManager::Get().Get(lights[0]);

    for (int i = 1; i < lights.size(); i++) {
        auto light_entity            = lights[i];
        auto light_component_current = LightComponentManager::Get().Get(light_entity);
        if (light_component_current->GetType() == ELightComponentType::DIRECTIONAL) {
            light_component = light_component_current;
            break;
        }
    }

    if (light_component->GetType() != ELightComponentType::DIRECTIONAL) {
        LOG_WARNING("No Directional Light found in the scene, exposure calculation will be skipped.");
        return nullptr;
    }
    auto* directional_light = dynamic_cast<DirectionalLightComponent*>(light_component.Get());
    if (directional_light == nullptr) {
        LOG_ERROR("LightComponent is not DirectionalLightComponent! This should not happen, code error.");
        return nullptr;
    }

    return directional_light;
}

} // namespace Moer::Render::Raster