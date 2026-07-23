#include "SkyboxPass.h"

// 实现 Skybox 曝光校正和全屏背景绘制。
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

SkyboxPass::RecordParameters SkyboxPass::Prepare(
    const RasterContext& context,
    const RasterConfig&  ui_config,
    const Camera&        camera
) const {
    RecordParameters parameters{};
    auto&            skybox_param = parameters.pass_param;
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

    parameters.bindless      = context.bdls;
    parameters.cubemap_owner = context.textures.cubemap_tex.tex;
    parameters.depth_owner   = context.textures.depth_linear_sampler.tex;
    parameters.output        = context.textures.lighting_output.tex;
    parameters.render_area   = context.textures.lighting_output.GetRect2D();
    return parameters;
}

void SkyboxPass::Record(CommandList& cmd_list, const RecordParameters& parameters) {
    DepthAttachment depth_attachment(parameters.depth_owner->GetView().GetTexture());
    depth_attachment.action = EAttachmentAction::AC_DS_LOAD_STORE;

    cmd_list.Gfx(skybox_pipeline, parameters.bindless, parameters.pass_param)
        .Draw(
            "Skybox Pass",
            parameters.render_area,
            std::move(RasterTool::GetFullScreenDrawDatas()),
            depth_attachment,
            // 保留上一个延迟光照 Pass 写入的结果。
            ColorAttachment{
                parameters.output, EAttachmentAction::AC_LOAD_STORE, float4(0, 0, 0, 0)
            }
        );
}

void SkyboxPass::Process(RasterContext& context, const RasterConfig& ui_config, const Camera& camera) {
    Record(context.cmd_list, Prepare(context, ui_config, camera));
}

} // namespace Moer::Render::Raster
