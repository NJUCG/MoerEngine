#pragma once

#include "scene/camera/Camera.h"
#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/raster/lighting_pass/ShaderParameters.h"

#include "RasterConfig.h"
#include "RasterResource.h"
#include "RasterTool.h"

namespace Moer::Render::Raster {

class PbrMaterialShadingPipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(PbrMaterialShadingPipeline);
    DEFINE_SHADER_BUFFER(lighting_data);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_CONSTANT_STRUCT(MaterialPassBindlessParam, param);
    DEFINE_SHADER_ARGS(lighting_data, bdls, param);
};

class LightingPass {
public:
    LightingPass(RasterContext& context) {
        //TODO： 开启深度模板测试提高性能
        //RHIDepthStencilStateInfo ds_info = RHIDepthStencilStateInfo::Preset<DepthStencil::DEPTH_WRITE>();

        // ds_info.b_enable_front_face_stencil = true;
        // ds_info.front_face_stencil_test     = ECompareOption::CO_EQUAL;
        // ds_info.front_face_pass_stencil_op  = EStencilOp::SO_KEEP;
        // ds_info.b_enable_back_face_stencil  = true;
        // ds_info.back_face_stencil_test      = ECompareOption::CO_EQUAL;
        // ds_info.back_face_pass_stencil_op   = EStencilOp::SO_KEEP;

        GfxPsoCreateInfo pso_full_screen_info(
            RHIRasterizeInfo::Preset(),
            {},
            {RHIColorAttachmentInfo::Preset(context.textures.lighting_output.tex->GetFormat())} //,ds_info,
            //context.textures.depth_linear_sampler.tex->GetFormat()
        );

        pbr_pipeline = context.manager.Raster()
                           .Vertex("core/utils/FullScreenQuad.hlsl")
                           .Pixel("pipelines/raster/deferred/lighting/RasterLightingPass.frag.hlsl")
                           .Build<PbrMaterialShadingPipeline>(std::move(pso_full_screen_info));
    }

    void Process(RasterContext& context, const RasterConfig& ui_config, const Camera& camera) {

        MaterialPassBindlessParam pass_param{};
        pass_param.extra_ambient_color     = ui_config.shading_extra_ambient_color;
        pass_param.extra_ambient_intensity = ui_config.shading_extra_ambient_intensity;
        pass_param.enable_extra_ambient    = ui_config.shading_enable_extra_ambient;
        pass_param.shading_mode            = static_cast<uint>(ui_config.shading_mode);

        pass_param.gbuffer_base_color     = context.textures.base_color.hdl;
        pass_param.gbuffer_normal         = context.textures.normal.hdl;
        pass_param.gbuffer_metal_rough_ao = context.textures.metal_rough_ao.hdl;
        pass_param.gbuffer_depth          = context.textures.depth_nearest_sampler.hdl;

        pass_param.light_buf_hdl      = context.scene.GetGpuSceneRes().light_buf.hdl;
        pass_param.cubemap_handle     = context.textures.cubemap_tex.hdl;
        pass_param.shadow_mask_handle = context.textures.shadow_mask.hdl;

        //context.cmd_list.SetStencilReference(1, 1);

        //未来希望能用该Attachment进行深度模板测试
        DepthAttachment depth_attachment =
            DepthAttachment(context.textures.depth_linear_sampler.tex->GetView().GetTexture());
        depth_attachment.action = AC_DS_LOAD_STORE;

        context.cmd_list.Gfx(pbr_pipeline, context.lighting_data_buffer.buf, context.bdls, pass_param)
            .Draw(
                "Lighting Pass",
                context.textures.lighting_output.GetRect2D(),
                std::move(RasterTool::GetFullScreenDrawDatas()),
                //depth_attachment,
                ColorAttachment(context.textures.lighting_output.tex)
            );
    }

private:
    PbrMaterialShadingPipeline pbr_pipeline;
};

} // namespace Moer::Render::Raster