#pragma once

#include "math/Function.h"
#include "scene/camera/Camera.h"
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

class RtaoPipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(RtaoPipeline);

    DEFINE_SHADER_CONSTANT_STRUCT(RtaoPipelineBindlessParam, param);
    DEFINE_SHADER_TEX(rw_ao_only);
    DEFINE_SHADER_TEX(rw_camera_mv);
    DEFINE_SHADER_TLAS(tlas);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);

    DEFINE_SHADER_ARGS(param, rw_ao_only, rw_camera_mv, tlas, bdls);

    MUTATION_BOOL(RTAO_COSINE_WEIGHTED);
};

MUTATION_SET(RtaoSampleModeMacros, RtaoPipeline::RTAO_COSINE_WEIGHTED);

class SsdoPipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(SsdoPipeline);
    DEFINE_SHADER_CONSTANT_STRUCT(SsdoPipelineBindlessParam, param);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_ARGS(bdls, param);
};

class AoCompositePipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(AoCompositePipeline);
    DEFINE_SHADER_CONSTANT_STRUCT(AoCompositeParam, param);
    DEFINE_SHADER_TEX(rw_output);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_ARGS(param, rw_output, bdls);
};

/**
 * MARK: AO Pass
 * 
 * AO Pass will calculate CameraMotionVector simultaneously.
 * When ao_half_resolution is enabled, all AO computation and denoising runs
 * at half resolution, and the result is bilinear-upsampled back to full res.
 */
class AoPass {
public:
    struct AoPassOutput {
        uint ao_only;     // bindless hdl, for composite
        uint ao_only_idx; // 0 or 1, for denoiser double-buffering
    };

    struct AoTextureSet {
        TextureWithHandle ao_only;
        TextureWithHandle camera_mv;
    };

    AoPass(RasterContext& context) {
        auto create_pso_func = [&]() {
            GfxPsoCreateInfo pso_full_screen_info(
                RHIRasterizeInfo::Preset(),
                {},
                {RHIColorAttachmentInfo::Preset(context.textures.ao_output_ambient_only.tex->GetFormat()),
                 RHIColorAttachmentInfo::Preset(context.textures.camera_motion_vector.tex->GetFormat())}
            );
            return pso_full_screen_info;
        };

        ao_pipeline = context.manager.Raster()
                          .Vertex("core/utils/FullScreenQuad.hlsl")
                          .Pixel("pipelines/postprocess/lighting_effects/Ao.hlsl")
                          .Build<AoPipeline>(std::move(create_pso_func()));

        {
            RtaoSampleModeMacros uniform_macros{};
            uniform_macros.SetMutation<RtaoPipeline::RTAO_COSINE_WEIGHTED>(false);
            rtao_pipeline_uniform = context.manager.Compute<RtaoPipeline>(
                "pipelines/postprocess/lighting_effects/Rtao.hlsl", uniform_macros
            );

            RtaoSampleModeMacros cosine_macros{};
            cosine_macros.SetMutation<RtaoPipeline::RTAO_COSINE_WEIGHTED>(true);
            rtao_pipeline_cosine = context.manager.Compute<RtaoPipeline>(
                "pipelines/postprocess/lighting_effects/Rtao.hlsl", cosine_macros
            );
        }

        ssdo_pipeline = context.manager.Raster()
                            .Vertex("core/utils/FullScreenQuad.hlsl")
                            .Pixel("pipelines/postprocess/lighting_effects/Ssdo.hlsl")
                            .Build<SsdoPipeline>(std::move(create_pso_func()));

        ao_composite_pipeline = context.manager.Compute<AoCompositePipeline>(
            "pipelines/postprocess/lighting_effects/AoComposite.hlsl"
        );

        CreateMotionVectorData(context);
    }

    // 创建CMV数据
    void CreateMotionVectorData(RasterContext& context) {
        camera_mv_data_in_gpu.buf = context.device.CreateBuffer<byte>(
            "Raster::CameraMotionVectorData",
            sizeof(CameraMotionVectorData),
            EBufferUsageFlags::UNORDERED_ACCESS
        );

        camera_mv_data_in_gpu.hdl = context.bdls->AllocateBuffer(camera_mv_data_in_gpu.buf->GetView());
    }

    // 更新CMV数据
    void UpdateMotionVectorData(RasterContext& context, const Camera& camera) {
        camera_mv_data_in_cpu.world2clip_prev = camera_mv_data_in_cpu.world2clip;
        camera_mv_data_in_cpu.world2clip      = Transpose(camera.GetViewProjectionMatrix());

        context.cmd_list.CopyFrom(
            std::span<byte>((byte*)&camera_mv_data_in_cpu, sizeof(CameraMotionVectorData)),
            camera_mv_data_in_gpu.buf->GetView()
        );
    }

    AoPassOutput
    Process(RasterContext& context, const RasterConfig& ui_config, const Camera& camera, uint64 frame_idx) {
        const bool half_res = ui_config.ao_half_resolution;

        TextureWithHandle ao_only_full = context.textures.ao_output_ambient_only;
        TextureWithHandle ao_only_half = context.textures.ao_output_ambient_only_half;
        static uint       ao_only_idx  = 0;
        ao_only_idx ^= 1;
        if (ao_only_idx) {
            ao_only_full = context.textures.ao_output_ambient_only_1;
            ao_only_half = context.textures.ao_output_ambient_only_1_half;
        }

        AoTextureSet tex_set;
        if (half_res) {
            tex_set.ao_only   = ao_only_half;
            tex_set.camera_mv = context.textures.camera_motion_vector_half;
        } else {
            tex_set.ao_only   = ao_only_full;
            tex_set.camera_mv = context.textures.camera_motion_vector;
        }

        if (ui_config.ao_mode == EAoMode::RTAO || ui_config.ao_mode == EAoMode::RTAO_AO_ONLY) {
            ProcessRtao(context, ui_config, camera, frame_idx, tex_set);
        } else if (ui_config.ao_mode == EAoMode::SSDO || ui_config.ao_mode == EAoMode::SSDO_AO_ONLY) {
            ProcessSsdo(context, ui_config, camera, frame_idx, tex_set);
        } else {
            ProcessAo(context, ui_config, camera, frame_idx, tex_set);
        }

        return AoPassOutput{
            .ao_only     = (half_res ? ao_only_half : ao_only_full).hdl,
            .ao_only_idx = ao_only_idx,
        };
    }

    void CompositeAo(RasterContext& context, const RasterConfig& ui_config, uint ao_only_hdl) {
        AoCompositeParam param;
        param.ao_tex              = ao_only_hdl;
        param.color_tex           = context.textures.lighting_output.hdl;
        param.ao_mode             = static_cast<uint>(ui_config.ao_mode);
        param.full_resolution     = float2(context.textures.ao_output.GetSize());
        param.inv_full_resolution = float2(1.0f) / param.full_resolution;

        uint2 res = uint2(context.textures.ao_output.GetSize());
        context.cmd_list.Compute(ao_composite_pipeline, param, context.textures.ao_output.tex, context.bdls)
            .Dispatch(uint3((res.x + 7u) / 8u, (res.y + 7u) / 8u, 1), "AO Composite Pass");
    }

    void ProcessAo(
        RasterContext&      context,
        const RasterConfig& ui_config,
        const Camera&       camera,
        uint64              frame_idx,
        AoTextureSet        tex_set
    ) {
        AoPipelineBindlessParam param;

        param.clip2world        = Transpose(camera.GetViewProjectionMatrixInv());
        param.inv_resolution    = float2(1.0f) / float2(tex_set.ao_only.GetSize());
        param.ssao_intensity    = ui_config.ssao_intensity;
        param.ssao_max_distance = ui_config.ssao_max_distance;
        param.ssao_sample_count = ui_config.ssao_spp;
        param.ssao_radius       = ui_config.ssao_sample_radius;
        param.ao_mode           = static_cast<uint32>(ui_config.ao_mode);
        param.normal_tex        = context.textures.normal.hdl;
        param.depth_tex         = context.textures.depth_linear_sampler.hdl;
        param.noise_tex         = context.textures.noise_tex.hdl;

        UpdateMotionVectorData(context, camera);
        param.camera_mv_data_handle = camera_mv_data_in_gpu.hdl;

        context.cmd_list.Gfx(ao_pipeline, context.bdls, param)
            .Draw(
                "AO Pass",
                tex_set.ao_only.GetRect2D(),
                std::move(RasterTool::GetFullScreenDrawDatas()),
                ColorAttachment(tex_set.ao_only.tex),
                ColorAttachment(tex_set.camera_mv.tex)
            );
    }

    void ProcessRtao(
        RasterContext&      context,
        const RasterConfig& ui_config,
        const Camera&       camera,
        uint64              frame_idx,
        AoTextureSet        tex_set
    ) {
        RtaoPipelineBindlessParam param;

        param.clip2world         = Transpose(camera.GetViewProjectionMatrixInv());
        param.frame_idx          = frame_idx;
        param.normal_tex         = context.textures.normal.hdl;
        param.depth_tex          = context.textures.depth_nearest_sampler.hdl;
        param.spp                = ui_config.rtao_spp;
        param.resolution         = float2(tex_set.ao_only.GetSize());
        param.inv_resolution     = float2(1.0) / param.resolution;
        param.ray_trace_distance = ui_config.rtao_ray_trace_distance;
        param.intensity          = ui_config.rtao_intensity;

        UpdateMotionVectorData(context, camera);
        param.camera_mv_data_handle = camera_mv_data_in_gpu.hdl;
        param.noise_tex             = context.textures.noise_tex.hdl;
        param.depth_tex_resolution  = float2(context.textures.depth_nearest_sampler.GetSize());

        auto& active_rtao_pipeline = (ui_config.rtao_sample_mode == ERtaoSampleMode::COSINE_WEIGHTED) ?
                                         rtao_pipeline_cosine :
                                         rtao_pipeline_uniform;

        uint2 res = uint2(tex_set.ao_only.GetSize());
        context.cmd_list
            .Compute(
                active_rtao_pipeline,
                param,
                tex_set.ao_only.tex,
                tex_set.camera_mv.tex,
                context.rt_scene()->GetTlas(),
                context.bdls
            )
            .Dispatch(uint3((res.x + 7u) / 8u, (res.y + 7u) / 8u, 1), "RTAO Compute Pass");
    }

    void ProcessSsdo(
        RasterContext&      context,
        const RasterConfig& ui_config,
        const Camera&       camera,
        uint64              frame_idx,
        AoTextureSet        tex_set
    ) {
        SsdoPipelineBindlessParam param;

        param.clip2world              = Transpose(camera.GetViewProjectionMatrixInv());
        param.inv_resolution          = float2(1.0f) / float2(tex_set.ao_only.GetSize());
        param.ssdo_sample_count       = ui_config.ssao_spp;
        param.ssdo_radius             = ui_config.ssdo_sample_radius;
        param.ssdo_max_distance       = ui_config.ssdo_max_distance;
        param.ssdo_intensity          = ui_config.ssao_intensity;
        param.ssdo_indirect_intensity = ui_config.ssdo_indirect_intensity;
        param.normal_tex              = context.textures.normal.hdl;
        param.depth_tex               = context.textures.depth_nearest_sampler.hdl;
        param.noise_tex               = context.textures.noise_tex.hdl;
        param.ao_mode                 = static_cast<uint32>(ui_config.ao_mode);
        param.ssdo_depth_bias         = ui_config.ssdo_depth_bias;
        param.input_image             = context.textures.lighting_output.hdl;
        param.world2clip              = Transpose(camera.GetViewProjectionMatrix());
        param.camera_position         = camera.GetPosition();

        UpdateMotionVectorData(context, camera);
        param.camera_mv_data_handle = camera_mv_data_in_gpu.hdl;

        context.cmd_list.Gfx(ssdo_pipeline, context.bdls, param)
            .Draw(
                "SSDO Pass",
                tex_set.ao_only.GetRect2D(),
                std::move(RasterTool::GetFullScreenDrawDatas()),
                ColorAttachment(tex_set.ao_only.tex),
                ColorAttachment(tex_set.camera_mv.tex)
            );
    }

private:
    AoPipeline          ao_pipeline;
    RtaoPipeline        rtao_pipeline_uniform;
    RtaoPipeline        rtao_pipeline_cosine;
    SsdoPipeline        ssdo_pipeline;
    AoCompositePipeline ao_composite_pipeline;

    CameraMotionVectorData camera_mv_data_in_cpu; // mv: motion vector
    BufferWithHandle       camera_mv_data_in_gpu; // mv: motion vector
    Matrix4x4f             world2clip_prev{};
};

} // namespace Moer::Render::Raster