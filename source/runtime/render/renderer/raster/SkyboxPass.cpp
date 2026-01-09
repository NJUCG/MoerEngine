#pragma once

#include "scene/Camera.h"
#include "scene/Material.h"
#include "scene/RenderableManager.h"

#include "RasterResource.h"
#include "RasterTextures.h"
#include "RasterTool.h"
#include "SkyboxPass.h"

namespace Moer::Render::Raster {
SkyboxPass::SkyboxPass(RasterContext& context) {
    RHIDepthStencilStateInfo ds_info(
        false,              //不写入
        CO_GREATER_OR_EQUAL //Reverse depth
    );

    GfxPsoCreateInfo pso_full_screen_info(
        RHIRasterizeInfo::Preset(),
        {},
        {RHIColorAttachmentInfo::Preset(
            context.textures.lighting_output.tex->GetFormat()
        )},                                                    // Color Attachment
        ds_info,                                               // 上面配置好的深度状态
        context.textures.depth_linear_sampler.tex->GetFormat() // Depth Format
    );

    skybox_pipeline = context.manager.Raster()
                          .Vertex("core/utils/FullScreenQuad.hlsl")
                          .Pixel("pipelines/raster/deferred/env_and_atmo/SkyboxPass.frag.hlsl")
                          .Build<SkyboxPipeline>(std::move(pso_full_screen_info));
}

void SkyboxPass::Process(RasterContext& context, const RasterConfig& ui_config, const CameraRef& camera) {
    SkyboxPassBindlessParam skybox_param;
    skybox_param.cubemap_handle  = context.cubemap_tex.handle;
    skybox_param.exposure_factor = ui_config.skybox_exposure_correct_enabled ?
                                       powf(10.0f, ui_config.skybox_exposure_correct_factor_log10) :
                                       1.0f;
    skybox_param.camera_pos      = camera->GetPosition();
    skybox_param.inv_view_proj   = Transpose(camera->GetViewProjectionMatrixInv());

    DepthAttachment depth_att(context.textures.depth_linear_sampler.tex->GetView().GetTexture());
    depth_att.action = EAttachmentAction::AC_DS_LOAD_STORE;

    context.cmd_list.Gfx(skybox_pipeline, context.bdls, skybox_param)
        .Draw(
            "Skybox Pass",
            context.textures.lighting_output.GetRect2D(),
            std::move(RasterTool::GetFullScreenDrawDatas()),
            depth_att, // 传入修改后的附件
            ColorAttachment{
                context.textures.lighting_output.tex, EAttachmentAction::AC_LOAD_STORE, float4(0, 0, 0, 0)
            } //防止清空
        );
}
} // namespace Moer::Render::Raster