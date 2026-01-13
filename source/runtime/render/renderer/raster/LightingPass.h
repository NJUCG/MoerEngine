#pragma once

#include "math/Function.h"
#include "misc/MMemory.h"
#include "scene/Camera.h"
#include "scene/Material.h"
#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/raster/lighting_pass/ShaderParameters.h"

#include "RasterConfig.h"
#include "RasterResource.h"
#include "RasterTool.h"

namespace Moer::Render::Raster {

class PbrMaterialShadingPipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(PbrMaterialShadingPipeline);
    DEFINE_SHADER_CONSTANT_STRUCT(MaterialPassBindlessParam, param);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_ARGS(bdls, param);
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

    void Process(RasterContext& context, const RasterConfig& ui_config, const CameraRef& camera) {

        MaterialPassBindlessParam material_param;
        material_param.extra_ambient_color     = ui_config.shading_extra_ambient_color;
        material_param.extra_ambient_intensity = ui_config.shading_extra_ambient_intensity;
        material_param.enable_extra_ambient    = ui_config.shading_enable_extra_ambient;
        material_param.shading_mode            = static_cast<uint>(ui_config.shading_mode);

        material_param.material_buffer     = context.gpu_material_info_handle;
        material_param.vbuffer             = context.textures.vbuffer.handle;
        material_param.gbuffer_normal      = context.textures.normal.handle;
        material_param.gbuffer_tangent     = context.textures.tangent.handle;
        material_param.gbuffer_uv          = context.textures.uv.handle;
        material_param.gbuffer_depth       = context.textures.depth_nearest_sampler.handle;
        material_param.gbuffer_position    = context.textures.position.handle;
        material_param.global_param_handle = context.lighting_data_buffer.handle;
        material_param.light_buffer        = context.gpu_light_info_handle;
        material_param.cubemap_handle      = context.textures.cubemap_tex.handle;
        material_param.shadow_mask_handle  = context.textures.shadow_mask.handle;

        //context.cmd_list.SetStencilReference(1, 1);

        Moer::UnorderedSet<EMaterialType> material_types = {EMaterialType::E_PBR_STANDARD};
        for (auto type : material_types) {
            material_param.material_type = uint(type);

            //未来希望能用该Attachment进行深度模板测试
            DepthAttachment depth_attachment =
                DepthAttachment(context.textures.depth_linear_sampler.tex->GetView().GetTexture());
            depth_attachment.action = AC_DS_LOAD_STORE;

            context.cmd_list.Gfx(pbr_pipeline, context.bdls, material_param)
                .Draw(
                    "Lighting Pass",
                    context.textures.lighting_output.GetRect2D(),
                    std::move(RasterTool::GetFullScreenDrawDatas()),
                    //depth_attachment,
                    ColorAttachment(context.textures.lighting_output.tex)
                );
        };
    }

private:
    PbrMaterialShadingPipeline pbr_pipeline;
};

} // namespace Moer::Render::Raster