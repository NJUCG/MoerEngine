#pragma once

// 对几何缓冲和方向光阴影遮罩应用延迟 PBR 光照。
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
    /**
     * Immutable recording input captured on the render thread. Bindless handles
     * alone do not own their resources, so every referenced buffer/texture is
     * retained until the recorded source reaches a terminal submit callback.
     */
    struct RecordParameters {
        MaterialPassBindlessParam pass_param{};
        BufferRef                 lighting_data{};
        BufferRef                 light_buffer_owner{};
        BindlessArrayRef          bindless{};
        TextureRef                base_color_owner{};
        TextureRef                normal_owner{};
        TextureRef                metal_rough_ao_owner{};
        DepthBufferRef            depth_owner{};
        TextureRef                cubemap_owner{};
        TextureRef                shadow_mask_owner{};
        TextureRef                output{};
        Rect2D                    render_area{};
    };

    LightingPass(RasterContext& context) {
        GfxPsoCreateInfo pipeline_info(
            RHIRasterizeInfo::Preset(),
            {},
            {RHIColorAttachmentInfo::Preset(context.textures.lighting_output.tex->GetFormat())}
        );

        pipeline = context.manager.Raster()
                       .Vertex("core/utils/FullScreenQuad.hlsl")
                       .Pixel("pipelines/raster/deferred/lighting/RasterLightingPass.frag.hlsl")
                       .Build<PbrMaterialShadingPipeline>(std::move(pipeline_info));
    }

    [[nodiscard]] RecordParameters
    Prepare(const RasterContext& context, const RasterConfig& ui_config) const {
        RecordParameters parameters{};
        auto&            pass_param = parameters.pass_param;
        pass_param.extra_ambient_color     = ui_config.shading_extra_ambient_color;
        pass_param.extra_ambient_intensity = ui_config.shading_extra_ambient_intensity;
        pass_param.enable_extra_ambient    = ui_config.shading_enable_extra_ambient;
        pass_param.shading_mode            = static_cast<uint>(ui_config.shading_mode);

        pass_param.gbuffer_base_color     = context.textures.base_color.hdl;
        pass_param.gbuffer_normal         = context.textures.normal.hdl;
        pass_param.gbuffer_metal_rough_ao = context.textures.metal_rough_ao.hdl;
        pass_param.gbuffer_depth          = context.textures.depth_nearest_sampler.hdl;

        const auto& gpu_scene_res     = context.GetGpuSceneRes();
        pass_param.light_buf_hdl      = gpu_scene_res.light_buf.hdl;
        pass_param.cubemap_handle     = context.textures.cubemap_tex.hdl;
        pass_param.shadow_mask_handle = context.textures.shadow_mask.hdl;

        parameters.lighting_data        = context.lighting_data_buffer.buf;
        parameters.light_buffer_owner   = gpu_scene_res.light_buf.buf;
        parameters.bindless             = context.bdls;
        parameters.base_color_owner     = context.textures.base_color.tex;
        parameters.normal_owner         = context.textures.normal.tex;
        parameters.metal_rough_ao_owner = context.textures.metal_rough_ao.tex;
        parameters.depth_owner          = context.textures.depth_nearest_sampler.tex;
        parameters.cubemap_owner        = context.textures.cubemap_tex.tex;
        parameters.shadow_mask_owner    = context.textures.shadow_mask.tex;
        parameters.output               = context.textures.lighting_output.tex;
        parameters.render_area          = context.textures.lighting_output.GetRect2D();
        return parameters;
    }

    void Record(CommandList& cmd_list, const RecordParameters& parameters) {
        cmd_list
            .Gfx(
                pipeline,
                parameters.lighting_data,
                parameters.bindless,
                parameters.pass_param
            )
            .Draw(
                "Lighting Pass",
                parameters.render_area,
                std::move(RasterTool::GetFullScreenDrawDatas()),
                ColorAttachment(parameters.output)
            );
    }

    void Process(RasterContext& context, const RasterConfig& ui_config) {
        Record(context.cmd_list, Prepare(context, ui_config));
    }

private:
    PbrMaterialShadingPipeline pipeline;
};

} // namespace Moer::Render::Raster
