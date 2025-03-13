#pragma once

#include "math/Function.h"
#include "misc/MMemory.h"
#include "scene/Camera.h"
#include "scene/Material.h"
#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/raster/lighting_pass/ShaderParameters.h"

#include "RasterResource.h"
#include "RasterTool.h"
#include "ui/raster_ui/RasterConfig.h"

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
        lighting_data_buffer.buf =
            context.device.CreateBuffer<byte>(sizeof(LightingData), EBufferUsageFlags::UNORDERED_ACCESS);

        lighting_data_buffer.handle = context.bdls->AllocateBuffer(lighting_data_buffer.buf->GetView());
    }

    uint Process(RasterContext& context, const RasterConfig& ui_config, const CameraRef& camera) {

        MaterialPassBindlessParam material_param;
        material_param.material_buffer     = context.gpu_material_info_handle;
        material_param.g_buffer_uv         = context.textures.uv.handle;
        material_param.g_buffer_normal     = context.textures.normal.handle;
        material_param.v_buffer            = context.textures.vbuffer.handle;
        material_param.g_buffer_depth      = context.textures.depth_nearest_sampler.handle;
        material_param.gbuffer_position    = context.textures.position.handle;
        material_param.global_param_handle = lighting_data_buffer.handle;
        material_param.light_buffer        = context.gpu_light_info_handle;

        // 注意生命周期！
        LightingData* lighting_data = MoerNew(LightingData);

        lighting_data->inv_view_proj   = Transpose(camera->GetViewProjectionMatrixInv());
        lighting_data->light_count     = context.scene.GetLights().size();
        lighting_data->camera_position = camera->GetPosition();

        context.cmd_list.CopyFrom(
            std::span<byte>((byte*)lighting_data, sizeof(LightingData)), lighting_data_buffer.buf->GetView()
        );
        context.cmd_list.AddCallback([lighting_data]() { MoerDelete(lighting_data); });

        Moer::UnorderedSet<EMaterialType> material_types = {EMaterialType::E_PBR_STANDARD};
        for (auto type : material_types) {
            material_param.material_type = uint(type);
            context.cmd_list.Gfx(pbr_pipeline, context.bdls, material_param)
                .Draw(
                    "Lighting Pass",
                    Rect2D(0, 0, context.resolution.x, context.resolution.y),
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