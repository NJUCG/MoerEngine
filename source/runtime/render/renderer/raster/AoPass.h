#pragma once

#include "math/Function.h"
#include "scene/camera/Camera.h"
#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/raster/post_process/ShaderParameters.h"

#include "RasterConfig.h"
#include "RasterResource.h"
#include "RasterTool.h"
#include "rendergraph/RenderGraph.h"

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
    struct GraphResources {
        RenderGraph::TextureHandle normal{};
        RenderGraph::TextureHandle depth{};
        RenderGraph::TextureHandle lighting_output{};
        RenderGraph::TokenHandle   scene{};
        RenderGraph::TokenHandle   ao_working_set{};
        RenderGraph::TokenHandle   motion_vectors{};
    };

    struct CompositeGraphResources {
        RenderGraph::TokenHandle   ao_working_set{};
        RenderGraph::TextureHandle lighting_output{};
        RenderGraph::TextureHandle depth{};
        RenderGraph::TextureHandle normal{};
        RenderGraph::TextureHandle ao_output{};
        RenderGraph::TokenHandle   processing_image{};
    };

    struct AoPassOutput {
        uint ao_only;     // bindless hdl, for composite
        uint ao_only_idx; // 0 or 1, for denoiser double-buffering
    };

    struct AoTextureSet {
        TextureWithHandle ao_only;
        TextureWithHandle camera_mv;
    };

    struct RecordParameters {
        EAoMode                       mode{EAoMode::NONE};
        ERtaoSampleMode               rtao_sample_mode{ERtaoSampleMode::UNIFORM};
        AoPassOutput                  output{};
        AoTextureSet                  textures{};
        BindlessArrayRef              bindless{};
        BufferWithHandle              motion_vector_data{};
        CameraMotionVectorData        motion_vector_upload{};
        RaytracingTlasRef              tlas{};
        AoPipelineBindlessParam        ao{};
        RtaoPipelineBindlessParam      rtao{};
        SsdoPipelineBindlessParam      ssdo{};
    };

    struct CompositeRecordParameters {
        AoCompositeParam  shader{};
        TextureWithHandle output{};
        BindlessArrayRef  bindless{};
        uint3             groups{};
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

    [[nodiscard]] uint NextAoOnlyIndex() const noexcept {
        return committed_ao_only_idx ^ 1u;
    }

    [[nodiscard]] AoPassOutput DescribeNextOutput(
        RasterContext&       context,
        const RasterConfig&  ui_config
    ) const {
        const bool half_res = ui_config.ao_half_resolution;
        TextureWithHandle ao_only_full = context.textures.ao_output_ambient_only;
        TextureWithHandle ao_only_half = context.textures.ao_output_ambient_only_half;
        const uint        ao_only_idx  = NextAoOnlyIndex();
        if (ao_only_idx) {
            ao_only_full = context.textures.ao_output_ambient_only_1;
            ao_only_half = context.textures.ao_output_ambient_only_1_half;
        }
        return AoPassOutput{
            .ao_only     = (half_res ? ao_only_half : ao_only_full).hdl,
            .ao_only_idx = ao_only_idx,
        };
    }

    [[nodiscard]] RecordParameters Prepare(
        RasterContext&       context,
        const RasterConfig&  ui_config,
        const Camera&        camera,
        uint64               frame_idx
    ) const {
        RecordParameters parameters{};
        parameters.mode             = ui_config.ao_mode;
        parameters.rtao_sample_mode = ui_config.rtao_sample_mode;
        parameters.bindless         = context.bdls;
        parameters.motion_vector_data = camera_mv_data_in_gpu;
        parameters.motion_vector_upload.world2clip_prev = camera_mv_data_in_cpu.world2clip;
        parameters.motion_vector_upload.world2clip =
            Transpose(camera.GetViewProjectionMatrix());
        parameters.output = DescribeNextOutput(context, ui_config);

        TextureWithHandle ao_only_full = context.textures.ao_output_ambient_only;
        TextureWithHandle ao_only_half = context.textures.ao_output_ambient_only_half;
        if (parameters.output.ao_only_idx != 0u) {
            ao_only_full = context.textures.ao_output_ambient_only_1;
            ao_only_half = context.textures.ao_output_ambient_only_1_half;
        }

        if (ui_config.ao_half_resolution) {
            parameters.textures.ao_only   = ao_only_half;
            parameters.textures.camera_mv = context.textures.camera_motion_vector_half;
        } else {
            parameters.textures.ao_only   = ao_only_full;
            parameters.textures.camera_mv = context.textures.camera_motion_vector;
        }

        {
            auto& param = parameters.ao;
            param.clip2world        = Transpose(camera.GetViewProjectionMatrixInv());
            param.inv_resolution = float2(1.0f) / float2(parameters.textures.ao_only.GetSize());
            param.ssao_intensity    = ui_config.ssao_intensity;
            param.ssao_max_distance = ui_config.ssao_max_distance;
            param.ssao_sample_count = ui_config.ssao_spp;
            param.ssao_radius       = ui_config.ssao_sample_radius;
            param.ao_mode           = static_cast<uint32>(ui_config.ao_mode);
            param.normal_tex        = context.textures.normal.hdl;
            param.depth_tex         = context.textures.depth_linear_sampler.hdl;
            param.noise_tex         = context.textures.noise_tex.hdl;
            param.camera_mv_data_handle = camera_mv_data_in_gpu.hdl;
        }
        {
            auto& param = parameters.rtao;
            param.clip2world         = Transpose(camera.GetViewProjectionMatrixInv());
            param.frame_idx          = frame_idx;
            param.normal_tex         = context.textures.normal.hdl;
            param.depth_tex          = context.textures.depth_nearest_sampler.hdl;
            param.spp                = ui_config.rtao_spp;
            param.resolution         = float2(parameters.textures.ao_only.GetSize());
            param.inv_resolution     = float2(1.0) / param.resolution;
            param.ray_trace_distance = ui_config.rtao_ray_trace_distance;
            param.intensity          = ui_config.rtao_intensity;
            param.camera_mv_data_handle = camera_mv_data_in_gpu.hdl;
            param.noise_tex             = context.textures.noise_tex.hdl;
            param.depth_tex_resolution  =
                float2(context.textures.depth_nearest_sampler.GetSize());
            if (context.rt_scene()) {
                parameters.tlas = context.rt_scene()->GetTlas();
            }
        }
        {
            auto& param = parameters.ssdo;
            param.clip2world = Transpose(camera.GetViewProjectionMatrixInv());
            param.inv_resolution = float2(1.0f) / float2(parameters.textures.ao_only.GetSize());
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

    void Record(CommandList& cmd_list, const RecordParameters& parameters) {
        CameraMotionVectorData upload = parameters.motion_vector_upload;
        cmd_list.CopyFrom(
            std::span<byte>(reinterpret_cast<byte*>(&upload), sizeof(upload)),
            parameters.motion_vector_data.buf->GetView()
        );

        if (parameters.mode == EAoMode::RTAO || parameters.mode == EAoMode::RTAO_AO_ONLY) {
            auto& active_pipeline =
                parameters.rtao_sample_mode == ERtaoSampleMode::COSINE_WEIGHTED ?
                    rtao_pipeline_cosine :
                    rtao_pipeline_uniform;
            const uint2 res = uint2(parameters.textures.ao_only.GetSize());
            cmd_list.Compute(
                        active_pipeline,
                        parameters.rtao,
                        parameters.textures.ao_only.tex,
                        parameters.textures.camera_mv.tex,
                        parameters.tlas,
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
                    parameters.textures.ao_only.GetRect2D(),
                    std::move(RasterTool::GetFullScreenDrawDatas()),
                    ColorAttachment(parameters.textures.ao_only.tex),
                    ColorAttachment(parameters.textures.camera_mv.tex)
                );
            return;
        }

        cmd_list.Gfx(ao_pipeline, parameters.bindless, parameters.ao)
            .Draw(
                "AO Pass",
                parameters.textures.ao_only.GetRect2D(),
                std::move(RasterTool::GetFullScreenDrawDatas()),
                ColorAttachment(parameters.textures.ao_only.tex),
                ColorAttachment(parameters.textures.camera_mv.tex)
            );
    }

    [[nodiscard]] RenderGraph::PreparedPassHandle AddToGraph(
        RenderGraph& graph,
        RasterContext& context,
        const RasterConfig& ui_config,
        const Camera& camera,
        uint64 frame_index,
        GraphResources resources
    );

    [[nodiscard]] CompositeRecordParameters PrepareComposite(
        RasterContext&       context,
        const RasterConfig&  ui_config,
        uint                 ao_only_hdl
    ) const {
        CompositeRecordParameters parameters{};
        auto& param = parameters.shader;
        param.ao_tex              = ao_only_hdl;
        param.color_tex           = context.textures.lighting_output.hdl;
        param.ao_mode             = static_cast<uint>(ui_config.ao_mode);
        param.is_half_resolution  = ui_config.ao_half_resolution ? 1u : 0u;
        param.full_resolution     = float2(context.textures.ao_output.GetSize());
        param.inv_full_resolution = float2(1.0f) / param.full_resolution;
        param.depth_tex           = context.textures.depth_nearest_sampler.hdl;
        param.normal_tex          = context.textures.normal.hdl;
        if (ui_config.ao_half_resolution) {
            const uint2 full = uint2(context.textures.ao_output.GetSize());
            param.ao_resolution = float2(std::max(1u, full.x / 2), std::max(1u, full.y / 2));
        } else {
            param.ao_resolution = param.full_resolution;
        }
        parameters.output   = context.textures.ao_output;
        parameters.bindless = context.bdls;
        const uint2 res      = uint2(parameters.output.GetSize());
        parameters.groups   = uint3((res.x + 7u) / 8u, (res.y + 7u) / 8u, 1);
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

    [[nodiscard]] RenderGraph::PreparedPassHandle AddCompositeToGraph(
        RenderGraph& graph,
        RasterContext& context,
        const RasterConfig& ui_config,
        uint ao_only_handle,
        CompositeGraphResources resources
    );

    void CommitFrame(const RecordParameters& parameters) {
        camera_mv_data_in_cpu = parameters.motion_vector_upload;
        committed_ao_only_idx = parameters.output.ao_only_idx;
    }

    void CommitFrame(const Matrix4x4f& view_proj, uint ao_only_idx) {
        camera_mv_data_in_cpu.world2clip_prev = camera_mv_data_in_cpu.world2clip;
        camera_mv_data_in_cpu.world2clip      = Transpose(view_proj);
        committed_ao_only_idx                 = ao_only_idx;
    }

    AoPassOutput Process(
        RasterContext& context,
        const RasterConfig& ui_config,
        const Camera& camera,
        uint64 frame_idx
    ) {
        const RecordParameters parameters = Prepare(context, ui_config, camera, frame_idx);
        Record(context.cmd_list, parameters);
        CommitFrame(parameters);
        return parameters.output;
    }

    void CompositeAo(RasterContext& context, const RasterConfig& ui_config, uint ao_only_hdl) {
        const CompositeRecordParameters parameters =
            PrepareComposite(context, ui_config, ao_only_hdl);
        RecordComposite(context.cmd_list, parameters);
    }

private:
    AoPipeline          ao_pipeline;
    RtaoPipeline        rtao_pipeline_uniform;
    RtaoPipeline        rtao_pipeline_cosine;
    SsdoPipeline        ssdo_pipeline;
    AoCompositePipeline ao_composite_pipeline;

    CameraMotionVectorData camera_mv_data_in_cpu{}; // mv: motion vector
    BufferWithHandle       camera_mv_data_in_gpu; // mv: motion vector
    uint                    committed_ao_only_idx{0};
};

} // namespace Moer::Render::Raster
