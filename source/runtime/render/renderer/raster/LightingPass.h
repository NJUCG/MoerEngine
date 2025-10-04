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
                           .Vertex("utils/FullScreenQuad.hlsl")
                           .Pixel("raster/lighting_pass/PbrMaterial.frag.hlsl")
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
        material_param.skybox_handle_posz  = context.skybox_tex[0].handle;
        material_param.skybox_handle_negz  = context.skybox_tex[1].handle;
        material_param.skybox_handle_posy  = context.skybox_tex[2].handle;
        material_param.skybox_handle_negy  = context.skybox_tex[3].handle;
        material_param.skybox_handle_posx  = context.skybox_tex[4].handle;
        material_param.skybox_handle_negx  = context.skybox_tex[5].handle;

        // 注意生命周期！
        LightingData* lighting_data = MoerNew(LightingData);

        lighting_data->inv_view_proj   = Transpose(camera->GetViewProjectionMatrixInv());
        lighting_data->light_count     = context.scene.GetLights().size();
        lighting_data->camera_position = camera->GetPosition();

        // Shadow Parameters
        lighting_data->shadow_map_mode            = ui_config.shadow_map_mode;
        lighting_data->shadow_sampling_mode       = ui_config.shadow_sampling_mode;
        lighting_data->shadow_csm_num_of_cascades = ui_config.shadow_csm_num_of_cascades;
        lighting_data->shadow_csm_sm_size         = ui_config.shadow_csm_sm_size;
        // Shadow Map
        lighting_data->shadow_map_0 = context.shadow_map_textures[0].handle;
        lighting_data->shadow_map_1 = context.shadow_map_textures[1].handle;
        lighting_data->shadow_map_2 = context.shadow_map_textures[2].handle;
        lighting_data->shadow_map_3 = context.shadow_map_textures[3].handle;
        // Shadow Transform
        lighting_data->world_to_shadow_clip_0 = Transpose(context.world_to_shadow_clip[0]);
        lighting_data->world_to_shadow_clip_1 = Transpose(context.world_to_shadow_clip[1]);
        lighting_data->world_to_shadow_clip_2 = Transpose(context.world_to_shadow_clip[2]);
        lighting_data->world_to_shadow_clip_3 = Transpose(context.world_to_shadow_clip[3]);
        // 注：此处不一定使用所有4张CSM，Shader中具体根据shadow_csm_num_of_cascades来决定

        context.cmd_list.CopyFrom(
            std::span<byte>((byte*)lighting_data, sizeof(LightingData)), lighting_data_buffer.buf->GetView()
        );
        context.cmd_list.AddCallback([lighting_data]() {
            MoerDelete(lighting_data);
        });

        Moer::UnorderedSet<EMaterialType> material_types = {EMaterialType::E_PBR_STANDARD};
        for (auto type : material_types) {
            material_param.material_type = uint(type);
            context.cmd_list.Gfx(pbr_pipeline, context.bdls, material_param)
                .Draw(
                    "Lighting Pass",
                    Rect2D(0, 0, context.resolution->x, context.resolution->y),
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