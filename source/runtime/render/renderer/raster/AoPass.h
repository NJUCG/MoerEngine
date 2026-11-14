#pragma once

#include "math/Function.h"
#include "misc/Types.h"
#include "scene/camera/Camera.h"
#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/raster/post_process/ShaderParameters.h"

#include "AoFrameResources.h"
#include "RasterConfig.h"
#include "RasterResource.h"
#include "RasterTool.h"
#include "rendergraph/RenderGraph.h"
#include <optional>

namespace Moer::Render::Raster {

class AoPipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(AoPipeline);
    DEFINE_SHADER_CONSTANT(AoPipelineBindlessParam, param);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_ARGS(bdls, param);
};

class RtaoPipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(RtaoPipeline);

    DEFINE_SHADER_CONSTANT(RtaoPipelineBindlessParam, param);
    DEFINE_SHADER_TEX(rw_ao_only);
    DEFINE_SHADER_TEX(rw_camera_mv);
    DEFINE_SHADER_TLAS(tlas);
    DEFINE_SHADER_TEX(rw_geometry);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);

    DEFINE_SHADER_ARGS(param, rw_ao_only, rw_camera_mv, tlas, rw_geometry, bdls);

    MUTATION_BOOL(RTAO_COSINE_WEIGHTED);
};

MUTATION_SET(RtaoSampleModeMacros, RtaoPipeline::RTAO_COSINE_WEIGHTED);

class SsdoPipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(SsdoPipeline);
    DEFINE_SHADER_CONSTANT(SsdoPipelineBindlessParam, param);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_ARGS(bdls, param);
};

class AoCompositePipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(AoCompositePipeline);
    DEFINE_SHADER_CONSTANT(AoCompositeParam, param);
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
    struct GraphResources {
        RenderGraph::TextureHandle normal{};
        RenderGraph::TextureHandle depth{};
        RenderGraph::TextureHandle lighting_output{};
        RenderGraph::TextureHandle ao_only{};
        RenderGraph::TextureHandle camera_motion_vector{};
        RenderGraph::BufferHandle  motion_vector_data{};
        RenderGraph::TextureHandle indirect{};
        RenderGraph::TextureHandle geometry{};
    };

    struct CompositeGraphResources {
        RenderGraph::TextureHandle ao_only{};
        RenderGraph::TextureHandle lighting_output{};
        RenderGraph::TextureHandle depth{};
        RenderGraph::TextureHandle normal{};
        RenderGraph::TextureHandle ao_output{};
        RenderGraph::TextureHandle indirect{};
    };

    struct RecordParameters {
        EAoMode                   mode{EAoMode::NONE};
        ERtaoSampleMode           rtao_sample_mode{ERtaoSampleMode::UNIFORM};
        AoFrameResources          frame{};
        BindlessArrayRef          bindless{};
        BufferWithHandle          motion_vector_data{};
        CameraMotionVectorData    motion_vector_upload{};
        RaytracingTlasRef         tlas{};
        AoPipelineBindlessParam   ao{};
        RtaoPipelineBindlessParam rtao{};
        SsdoPipelineBindlessParam ssdo{};
    };

    struct CompositeRecordParameters {
        AoCompositeParam  shader{};
        TextureWithHandle output{};
        BindlessArrayRef  bindless{};
        uint3             groups{};
    };

    AoPass(RasterContext& context) {
        auto create_pso_func = [&](bool include_indirect = false) {
            Array<RHIColorAttachmentInfo> attachments{
                RHIColorAttachmentInfo::Preset(context.textures.ao_output_ambient_only.tex->GetFormat()),
                RHIColorAttachmentInfo::Preset(context.textures.camera_motion_vector.tex->GetFormat())
            };
            if (include_indirect) {
                attachments.push_back(RHIColorAttachmentInfo::Preset(context.textures.ao_indirect.tex->GetFormat()));
            }
            GfxPsoCreateInfo pso_full_screen_info(
                RHIRasterizeInfo::Preset(),
                {},
                std::move(attachments)
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
                            .Build<SsdoPipeline>(create_pso_func(true));

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

    [[nodiscard]] const BufferRef& GetMotionVectorDataBuffer() const {
        return camera_mv_data_in_gpu.buf;
    }

    [[nodiscard]] AoFrameResources SelectFrameResources(
        const RasterContext& context,
        const RasterConfig&  config
    ) const {
        const auto&      textures = context.textures;
        AoFrameResources frame{};
        frame.half_resolution = config.ao_half_resolution;
        frame.denoise         = config.rtao_denoiser_enable &&
                        (config.ao_mode == EAoMode::RTAO || config.ao_mode == EAoMode::RTAO_AO_ONLY);
        frame.write_index = accepted_frame ? accepted_frame->write_index ^ 1u : 0u;
        if (frame.half_resolution) {
            frame.final_ao      = {textures.ao_output_ambient_only_half, textures.ao_output_ambient_only_1_half};
            frame.geometry      = {textures.ao_history_geometry_half, textures.ao_history_geometry_1_half};
            frame.noisy_ao      = textures.ao_noisy_half;
            frame.camera_motion = textures.camera_motion_vector_half;
            frame.indirect      = textures.ao_indirect_half;
        } else {
            frame.final_ao      = {textures.ao_output_ambient_only, textures.ao_output_ambient_only_1};
            frame.geometry      = {textures.ao_history_geometry, textures.ao_history_geometry_1};
            frame.noisy_ao      = textures.ao_noisy;
            frame.camera_motion = textures.camera_motion_vector;
            frame.indirect      = textures.ao_indirect;
        }
        const auto size           = frame.Output().GetSize();
        frame.has_previous_output = accepted_frame &&
                                    accepted_frame->half_resolution == frame.half_resolution &&
                                    accepted_frame->resolution.x == size.x && accepted_frame->resolution.y == size.y;
        frame.has_denoised_history = frame.has_previous_output && accepted_frame->denoised;
        return frame;
    }

    [[nodiscard]] RecordParameters Prepare(
        RasterContext&          context,
        const RasterConfig&     ui_config,
        const Camera&           camera,
        uint64                  frame_idx,
        const AoFrameResources& frame
    ) const {
        RecordParameters parameters{};
        parameters.mode                                 = ui_config.ao_mode;
        parameters.rtao_sample_mode                     = ui_config.rtao_sample_mode;
        parameters.bindless                             = context.bdls;
        parameters.motion_vector_data                   = camera_mv_data_in_gpu;
        parameters.motion_vector_upload.world2clip_prev = Transpose(
            accepted_frame ? accepted_frame->view_projection : camera.GetViewProjectionMatrix()
        );
        parameters.motion_vector_upload.world2clip =
            Transpose(camera.GetViewProjectionMatrix());
        parameters.frame = frame;

        {
            auto& param                 = parameters.ao;
            param.clip2world            = Transpose(camera.GetViewProjectionMatrixInv());
            param.inv_resolution        = float2(1.0f) / float2(parameters.frame.EvaluationOutput().GetSize());
            param.ssao_intensity        = ui_config.ssao_intensity;
            param.ssao_max_distance     = ui_config.ssao_max_distance;
            param.ssao_sample_count     = ui_config.ssao_spp;
            param.ssao_radius           = ui_config.ssao_sample_radius;
            param.ao_mode               = static_cast<uint32>(ui_config.ao_mode);
            param.normal_tex            = context.textures.normal.hdl;
            param.depth_tex             = context.textures.depth_linear_sampler.hdl;
            param.noise_tex             = context.textures.noise_tex.hdl;
            param.camera_mv_data_handle = camera_mv_data_in_gpu.hdl;
        }
        {
            auto& param                 = parameters.rtao;
            param.clip2world            = Transpose(camera.GetViewProjectionMatrixInv());
            param.frame_idx             = frame_idx;
            param.normal_tex            = context.textures.normal.hdl;
            param.depth_tex             = context.textures.depth_nearest_sampler.hdl;
            param.spp                   = ui_config.rtao_spp;
            param.resolution            = float2(parameters.frame.EvaluationOutput().GetSize());
            param.inv_resolution        = float2(1.0) / param.resolution;
            param.ray_trace_distance    = ui_config.rtao_ray_trace_distance;
            param.intensity             = ui_config.rtao_intensity;
            param.camera_mv_data_handle = camera_mv_data_in_gpu.hdl;
            param.noise_tex             = context.textures.noise_tex.hdl;
            param.depth_tex_resolution =
                float2(context.textures.depth_nearest_sampler.GetSize());
            if (context.rt_scene()) {
                parameters.tlas = context.rt_scene()->GetTlas();
            }
        }
        {
            auto& param                   = parameters.ssdo;
            param.clip2world              = Transpose(camera.GetViewProjectionMatrixInv());
            param.inv_resolution          = float2(1.0f) / float2(parameters.frame.EvaluationOutput().GetSize());
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
            param.camera_mv_data_handle   = camera_mv_data_in_gpu.hdl;
        }
        return parameters;
    }

    void RecordUpload(CommandList& cmd_list, const RecordParameters& parameters) {
        CameraMotionVectorData upload = parameters.motion_vector_upload;
        cmd_list.CopyFrom(
            std::span<byte>(reinterpret_cast<byte*>(&upload), sizeof(upload)),
            parameters.motion_vector_data.buf->GetView()
        );
    }

    void RecordEvaluate(CommandList& cmd_list, const RecordParameters& parameters) {

        if (parameters.mode == EAoMode::RTAO || parameters.mode == EAoMode::RTAO_AO_ONLY) {
            auto& active_pipeline =
                parameters.rtao_sample_mode == ERtaoSampleMode::COSINE_WEIGHTED ?
                    rtao_pipeline_cosine :
                    rtao_pipeline_uniform;
            const uint2 res = uint2(parameters.frame.EvaluationOutput().GetSize());
            cmd_list.Compute(
                        active_pipeline,
                        parameters.rtao,
                        parameters.frame.EvaluationOutput().tex,
                        parameters.frame.camera_motion.tex,
                        parameters.tlas,
                        parameters.frame.CurrentGeometry().tex,
                        parameters.bindless
            )
                .Dispatch(
                    uint3((res.x + 7u) / 8u, (res.y + 7u) / 8u, 1),
                    "RTAO Compute Pass"
                );
            return;
        }

        if (parameters.mode == EAoMode::SSDO || parameters.mode == EAoMode::SSDO_AO_ONLY) {
            cmd_list.Gfx(ssdo_pipeline, parameters.bindless, parameters.ssdo)
                .Draw(
                    "SSDO Pass",
                    parameters.frame.EvaluationOutput().GetRect2D(),
                    std::move(RasterTool::GetFullScreenDrawDatas()),
                    ColorAttachment(parameters.frame.EvaluationOutput().tex),
                    ColorAttachment(parameters.frame.camera_motion.tex),
                    ColorAttachment(parameters.frame.indirect.tex)
                );
            return;
        }

        cmd_list.Gfx(ao_pipeline, parameters.bindless, parameters.ao)
            .Draw(
                "AO Pass",
                parameters.frame.EvaluationOutput().GetRect2D(),
                std::move(RasterTool::GetFullScreenDrawDatas()),
                ColorAttachment(parameters.frame.EvaluationOutput().tex),
                ColorAttachment(parameters.frame.camera_motion.tex)
            );
    }

    void AddToGraph(
        RenderGraph&        graph,
        RasterContext&      context,
        const RasterConfig& ui_config,
        const Camera&       camera,
        uint64              frame_index,
        AoFrameResources    frame,
        GraphResources      resources
    );

    [[nodiscard]] CompositeRecordParameters PrepareComposite(
        RasterContext&          context,
        const RasterConfig&     ui_config,
        const AoFrameResources& frame
    ) const {
        CompositeRecordParameters parameters{};
        auto&                     param = parameters.shader;
        param.ao_tex                    = frame.Output().hdl;
        param.color_tex                 = context.textures.lighting_output.hdl;
        param.ao_mode                   = static_cast<uint32>(ui_config.ao_mode);
        param.is_half_resolution        = frame.half_resolution ? 1u : 0u;
        param.full_resolution           = float2(context.textures.ao_output.GetSize());
        param.inv_full_resolution       = float2(1.0f) / param.full_resolution;
        param.depth_tex                 = context.textures.depth_nearest_sampler.hdl;
        param.normal_tex                = context.textures.normal.hdl;
        param.indirect_tex              = frame.indirect.hdl;
        param.ao_resolution             = float2(frame.Output().GetSize());
        parameters.output               = context.textures.ao_output;
        parameters.bindless             = context.bdls;
        const uint2 res                 = uint2(parameters.output.GetSize());
        parameters.groups               = uint3((res.x + 7u) / 8u, (res.y + 7u) / 8u, 1);
        return parameters;
    }

    void RecordComposite(CommandList& cmd_list, const CompositeRecordParameters& parameters) {
        cmd_list.Compute(
                    ao_composite_pipeline,
                    parameters.shader,
                    parameters.output.tex,
                    parameters.bindless
        )
            .Dispatch(parameters.groups, "AO Composite Pass");
    }

    void AddCompositeToGraph(
        RenderGraph&            graph,
        RasterContext&          context,
        const RasterConfig&     ui_config,
        AoFrameResources        frame,
        CompositeGraphResources resources
    );

    // Called only after native submission acceptance. Rejected frames never
    // become temporal history, even if their prefix was already recorded.
    void CommitFrame(const AoFrameResources& frame, const Matrix4x4f& view_projection) {
        accepted_frame = AcceptedFrame{
            frame.write_index, uint2(frame.Output().GetSize()), frame.half_resolution, frame.denoise, view_projection
        };
    }

    void ResetHistory() {
        accepted_frame.reset();
    }

private:
    AoPipeline          ao_pipeline;
    RtaoPipeline        rtao_pipeline_uniform;
    RtaoPipeline        rtao_pipeline_cosine;
    SsdoPipeline        ssdo_pipeline;
    AoCompositePipeline ao_composite_pipeline;

    struct AcceptedFrame {
        uint32     write_index;
        uint2      resolution;
        bool       half_resolution;
        bool       denoised;
        Matrix4x4f view_projection;
    };
    std::optional<AcceptedFrame> accepted_frame;
    BufferWithHandle             camera_mv_data_in_gpu;
};

} // namespace Moer::Render::Raster
