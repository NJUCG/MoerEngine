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

        GfxPsoCreateInfo pso_full_screen_info(
            RHIRasterizeInfo::Preset(),
            {},
            {RHIColorAttachmentInfo::Preset(context.textures.lighting_output.tex->GetFormat())}
        );

        pbr_pipeline = context.manager.Raster()
                           .Vertex("core/utils/FullScreenQuad.hlsl")
                           .Pixel("pipelines/raster/deferred/lighting/RasterLightingPass.frag.hlsl")
                           .Build<PbrMaterialShadingPipeline>(std::move(pso_full_screen_info));

        CreateLightingData(context);
    }

    void CreateLightingData(RasterContext& context) {
        lighting_data_buffer.buf = context.device.CreateBuffer<byte>(
            "Raster::LightData", sizeof(LightingData), EBufferUsageFlags::UNORDERED_ACCESS
        );

        lighting_data_buffer.handle = context.bdls->AllocateBuffer(lighting_data_buffer.buf->GetView());
    }

    uint Process(RasterContext& context, const RasterConfig& ui_config, const CameraRef& camera) {

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
        material_param.global_param_handle = lighting_data_buffer.handle;
        material_param.light_buffer        = context.gpu_light_info_handle;
        for (int i = 0; i < 6; i++) {
            material_param.skybox_handles[i] = context.skybox_tex[i].handle;
        }

        {
            // 注意生命周期！
            LightingData* lighting_data = MoerNew(LightingData);
            uint          csm_layers    = ui_config.shadow_csm_num_of_cascades;

            lighting_data->inv_view_proj   = Transpose(camera->GetViewProjectionMatrixInv());
            lighting_data->light_count     = context.scene.GetLights().size();
            lighting_data->camera_position = camera->GetPosition();

            // Shadow Parameters
            lighting_data->shadow_map_mode              = static_cast<int>(ui_config.shadow_map_mode);
            lighting_data->shadow_sampling_mode         = ui_config.shadow_sampling_mode;
            lighting_data->shadow_csm_num_of_cascades   = csm_layers;
            lighting_data->shadow_csm_sm_size           = ui_config.shadow_csm_sm_size;
            lighting_data->shadow_csm_visualize_cascade = ui_config.shadow_csm_visualize_cascade;
            // Shadow Map
            for (uint i = 0; i < csm_layers; i++) {
                lighting_data->shadow_map[i] = context.shadow_map_data.shadow_map_textures[i].handle;
            }
            // Shadow Transform
            for (uint i = 0; i < csm_layers; i++) {
                lighting_data->world_to_shadow_clip[i] =
                    Transpose(context.shadow_map_data.world_to_shadow_clip[i]);
            }
            lighting_data->view_matrix = Transpose(camera->GetViewMatrix());
            lighting_data->near_clip   = camera->GetNearClip();
            lighting_data->far_clip    = camera->GetFarClip();
            for (int i = 0; i < csm_layers; i++) {
                lighting_data->cascade_split_ratios[i] = context.shadow_map_data.cascade_split_ratios[i];
            }
            for (int i = 0; i < csm_layers; i++) {
                lighting_data->cascade_blend_start_ratios[i] =
                    context.shadow_map_data.cascade_blend_start_ratios[i];
            }
            lighting_data->is_csm_blend_enabled = ui_config.shadow_csm_blend_option ? 1 : 0;
            // 注：此处不一定使用所有CSM，Shader中具体根据shadow_csm_num_of_cascades来决定

            // PCSS
            lighting_data->light_size_world =
                ui_config.shadow_pcss_light_size_world; //假定的光源大小，用于软阴影计算
            lighting_data->pcss_enabled = ui_config.shadow_pcss_enabled ? 1 : 0;
            for (uint i = 0; i < csm_layers; i++) {
                lighting_data->scale_data[i] = context.shadow_map_data.scaleDatas[i];
            }

            lighting_data->main_light_direction = context.shadow_map_data.light_dir;

            lighting_data->lut_ggx_emu_handle  = context.lut_ggx_emu.handle;
            lighting_data->lut_ggx_eavg_handle = context.lut_ggx_eavg.handle;

            context.cmd_list.CopyFrom(
                std::span<byte>((byte*)lighting_data, sizeof(LightingData)),
                lighting_data_buffer.buf->GetView()
            );
            context.cmd_list.AddCallback([lighting_data]() {
                MoerDelete(lighting_data);
            });
        }

        Moer::UnorderedSet<EMaterialType> material_types = {EMaterialType::E_PBR_STANDARD};
        for (auto type : material_types) {
            material_param.material_type = uint(type);
            context.cmd_list.Gfx(pbr_pipeline, context.bdls, material_param)
                .Draw(
                    "Lighting Pass",
                    context.textures.lighting_output.GetRect2D(),
                    std::move(RasterTool::GetFullScreenDrawDatas()),
                    ColorAttachment(context.textures.lighting_output.tex)
                );
        };

        return context.textures.lighting_output.handle;
    }

private:
    PbrMaterialShadingPipeline pbr_pipeline;

    BufferWithHandle lighting_data_buffer;
};

} // namespace Moer::Render::Raster