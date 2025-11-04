#pragma once

#include "math/Function.h"
#include "scene/Camera.h"
#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/raster/post_process/ShaderParameters.h"

#include "RasterConfig.h"
#include "RasterResource.h"
#include "RasterTool.h"

namespace Moer::Render::Raster {

class AoPipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(AoPipeline);
    DEFINE_SHADER_CONSTANT_STRUCT(AoPipelineBindlessParam, param);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_ARGS(bdls, param);
};

class RtaoPipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(RtaoPipeline);

    DEFINE_SHADER_TLAS(tlas);
    DEFINE_SHADER_CONSTANT_STRUCT(RtaoPipelineBindlessParam, param);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);

    DEFINE_SHADER_ARGS(tlas, bdls, param);
};

class SsdoPipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(SsdoPipeline);
    DEFINE_SHADER_CONSTANT_STRUCT(SsdoPipelineBindlessParam, param);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_ARGS(bdls, param);
};

/**
 * MARK: AO Pass
 * 
 * AO Pass will calculate CameraMotionVector simultaneously.
 * 
 * TODO: SSDO Support
 */
class AoPass {
public:
    struct AoPassOutput {
        uint ao_with_color;
        uint ao_only;
        uint ao_only_idx; // 0 or 1 表示ao_only的顺序
        uint camera_motion_vector;
    };

    AoPass(RasterContext& context) {
        auto create_pso_func = [&]() {
            GfxPsoCreateInfo pso_full_screen_info(
                RHIRasterizeInfo::Preset(),
                {},
                {RHIColorAttachmentInfo::Preset(context.textures.ao_output.tex->GetFormat()),
                 RHIColorAttachmentInfo::Preset(context.textures.ao_output_ambient_only.tex->GetFormat()),
                 RHIColorAttachmentInfo::Preset(context.textures.camera_motion_vector.tex->GetFormat())}
            );
            return pso_full_screen_info;
        };

        ao_pipeline = context.manager.Raster()
                          .Vertex("utils/FullScreenQuad.hlsl")
                          .Pixel("raster/post_process/Ao.hlsl")
                          .Build<AoPipeline>(std::move(create_pso_func()));

        rtao_pipeline = context.manager.Raster()
                            .Vertex("utils/FullScreenQuad.hlsl")
                            .Pixel("raster/post_process/Rtao.hlsl")
                            .Build<RtaoPipeline>(std::move(create_pso_func()));

        ssdo_pipeline = context.manager.Raster()
                            .Vertex("utils/FullScreenQuad.hlsl")
                            .Pixel("raster/post_process/Ssdo.hlsl")
                            .Build<SsdoPipeline>(std::move(create_pso_func()));

        CreateMotionVectorData(context);
    }

    // 创建CMV数据
    void CreateMotionVectorData(RasterContext& context) {
        camera_mv_data_in_gpu.buf = context.device.CreateBuffer<byte>(
            "Raster::CameraMotionVectorData",
            sizeof(CameraMotionVectorData),
            EBufferUsageFlags::UNORDERED_ACCESS
        );

        camera_mv_data_in_gpu.handle = context.bdls->AllocateBuffer(camera_mv_data_in_gpu.buf->GetView());
    }

    // 更新CMV数据
    void UpdateMotionVectorData(RasterContext& context, const CameraRef& camera) {
        camera_mv_data_in_cpu.world2clip_prev = camera_mv_data_in_cpu.world2clip;
        camera_mv_data_in_cpu.world2clip      = Transpose(camera->GetViewProjectionMatrix());

        context.cmd_list.CopyFrom(
            std::span<byte>((byte*)&camera_mv_data_in_cpu, sizeof(CameraMotionVectorData)),
            camera_mv_data_in_gpu.buf->GetView()
        );
    }

    AoPassOutput Process(
        RasterContext&      context,
        const RasterConfig& ui_config,
        const CameraRef&    camera,
        uint64              frame_idx,
        uint                input_image
    ) {
        TextureWithHandle ao_only     = context.textures.ao_output_ambient_only;
        static uint       ao_only_idx = 0;
        ao_only_idx ^= 1;
        if (ao_only_idx) {
            ao_only = context.textures.ao_output_ambient_only_1;
        }

        if (ui_config.ao_mode == EAoMode::RTAO || ui_config.ao_mode == EAoMode::RTAO_AO_ONLY) {
            ProcessRtao(context, ui_config, camera, frame_idx, input_image, ao_only);
        } else if (ui_config.ao_mode == EAoMode::SSDO || ui_config.ao_mode == EAoMode::SSDO_AO_ONLY) {
            ProcessSsdo(context, ui_config, camera, frame_idx, input_image, ao_only);
        } else {
            ProcessAo(context, ui_config, camera, frame_idx, input_image, ao_only);
        }

        return AoPassOutput{
            .ao_with_color        = context.textures.ao_output.handle,            //
            .ao_only              = ao_only.handle,                               //
            .ao_only_idx          = ao_only_idx,                                  //
            .camera_motion_vector = context.textures.camera_motion_vector.handle, //
        };
    }

    void ProcessAo(
        RasterContext&      context,
        const RasterConfig& ui_config,
        const CameraRef&    camera,
        uint64              frame_idx,
        uint                input_image,
        TextureWithHandle   ao_only
    ) {
        AoPipelineBindlessParam param;

        param.inv_resolution    = float2(1.0f) / float2(context.textures.ao_output.GetSize());
        param.ssao_intensity    = ui_config.ssao_intensity;
        param.ssao_max_distance = ui_config.ssao_max_distance;
        param.ssao_sample_count = ui_config.ssao_spp;
        param.ssao_radius       = ui_config.ssao_sample_radius;
        param.ao_mode           = static_cast<uint32>(ui_config.ao_mode);
        param.input_image       = input_image;
        param.normal_tex        = context.textures.normal.handle;
        param.position_tex      = context.textures.position.handle;
        param.depth_tex         = context.textures.depth_nearest_sampler.handle;
        param.noise_tex         = context.noise_tex.handle;

        UpdateMotionVectorData(context, camera);
        param.camera_mv_data_handle = camera_mv_data_in_gpu.handle;

        context.cmd_list.Gfx(ao_pipeline, context.bdls, param)
            .Draw(
                "AO Pass",
                context.textures.ao_output.GetRect2D(),
                std::move(RasterTool::GetFullScreenDrawDatas()),
                ColorAttachment(context.textures.ao_output.tex),
                ColorAttachment(ao_only.tex),
                ColorAttachment(context.textures.camera_motion_vector.tex)
            );
    }

    void ProcessRtao(
        RasterContext&      context,
        const RasterConfig& ui_config,
        const CameraRef&    camera,
        uint64              frame_idx,
        uint                input_image,
        TextureWithHandle   ao_only
    ) {

        RtaoPipelineBindlessParam param;

        param.clip2world         = Transpose(camera->GetViewProjectionMatrixInv());
        param.camera_pos         = camera->GetPosition();
        param.frame_idx          = frame_idx;
        param.resolution         = float2(context.textures.ao_output.GetSize());
        param.inv_resolution     = float2(1.0) / float2(context.textures.ao_output.GetSize());
        param.input_image        = input_image;
        param.normal_tex         = context.textures.normal.handle;
        param.position_tex       = context.textures.position.handle;
        param.depth_tex          = context.textures.depth_nearest_sampler.handle;
        param.ao_mode            = static_cast<uint>(ui_config.ao_mode);
        param.sample_mode        = static_cast<uint>(ui_config.rtao_sample_mode);
        param.spp                = ui_config.rtao_spp;
        param.ray_trace_distance = ui_config.rtao_ray_trace_distance;
        param.intensity          = ui_config.rtao_intensity;

        UpdateMotionVectorData(context, camera);
        param.camera_mv_data_handle = camera_mv_data_in_gpu.handle;

        context.cmd_list.Gfx(rtao_pipeline, context.rt_scene->GetTlas(), context.bdls, param)
            .Draw(
                "RTAO Pass",
                context.textures.ao_output.GetRect2D(),
                std::move(RasterTool::GetFullScreenDrawDatas()),
                ColorAttachment(context.textures.ao_output.tex),
                ColorAttachment(ao_only.tex),
                ColorAttachment(context.textures.camera_motion_vector.tex)
            );
    }

    void ProcessSsdo(
        RasterContext&      context,
        const RasterConfig& ui_config,
        const CameraRef&    camera,
        uint64              frame_idx,
        uint                input_image,
        TextureWithHandle   ao_only
    ) {
        SsdoPipelineBindlessParam param;

        param.inv_resolution         = float2(1.0f) / float2(context.textures.ao_output.GetSize());
        param.ssdo_sample_count      = ui_config.ssao_spp;
        param.ssdo_radius            = ui_config.ssao_sample_radius;
        param.ssdo_max_distance      = ui_config.ssao_max_distance;
        param.ssdo_intensity         = ui_config.ssao_intensity;
        param.normal_tex             = context.textures.normal.handle;
        param.depth_tex              = context.textures.depth_nearest_sampler.handle;
        param.position_tex           = context.textures.position.handle;
        param.noise_tex              = context.noise_tex.handle;
        param.ao_mode                = static_cast<uint32>(ui_config.ao_mode);
        param.ssdo_depth_bias        = ui_config.ssdo_depth_bias;
        param.input_image            = input_image;
        param.view_projection_matrix = camera->GetViewProjectionMatrix();
        param.camera_position        = camera->GetPosition();

        UpdateMotionVectorData(context, camera);
        param.camera_mv_data_handle = camera_mv_data_in_gpu.handle;

        context.cmd_list.Gfx(ssdo_pipeline, context.bdls, param)
            .Draw(
                "SSDO Pass",
                context.textures.ao_output.GetRect2D(),
                std::move(RasterTool::GetFullScreenDrawDatas()),
                ColorAttachment(context.textures.ao_output.tex),
                ColorAttachment(ao_only.tex),
                ColorAttachment(context.textures.camera_motion_vector.tex)
            );
    }

private:
    AoPipeline   ao_pipeline;
    RtaoPipeline rtao_pipeline;
    SsdoPipeline ssdo_pipeline;

    CameraMotionVectorData camera_mv_data_in_cpu; // mv: motion vector
    BufferWithHandle       camera_mv_data_in_gpu; // mv: motion vector
    Matrix4x4f             world2clip_prev{};
};

} // namespace Moer::Render::Raster