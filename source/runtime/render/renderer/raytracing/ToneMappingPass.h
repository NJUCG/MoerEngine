#ifndef MOER_TONE_MAPPING_PASS_H
#define MOER_TONE_MAPPING_PASS_H

// 计算亮度曝光，并将 HDR 光照映射到显示目标。

#include "RaytracingGraphResources.h"
#include "rhi/RHIResource.h"
#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/ShaderParameters.h"
#include "shaderheaders/shared/postprocess/ShaderParameters.h"

namespace Moer::Render {
class CommandList;
class RenderDevice;
class ShaderManager;
} // namespace Moer::Render

namespace Moer::Render::Raytracing {

class RENDER_API ToneMappingPassPipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(ToneMappingPassPipeline);

    DEFINE_SHADER_BUFFER(params);
    DEFINE_SHADER_TEX(source_tex);
    DEFINE_SHADER_BUFFER(exposure);
    DEFINE_SHADER_TEX(color_lut);
    DEFINE_SHADER_SAMPLER(color_lut_sampler);

    DEFINE_SHADER_ARGS(params, source_tex, exposure, color_lut, color_lut_sampler);
};

class HistogramPipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(HistogramPipeline);

    DEFINE_SHADER_BUFFER(params);
    DEFINE_SHADER_TEX(source_tex);
    DEFINE_SHADER_BUFFER(histogram);

    DEFINE_SHADER_ARGS(params, source_tex, histogram);
};

class ExposurePipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(ExposurePipeline);

    DEFINE_SHADER_BUFFER(params);
    DEFINE_SHADER_BUFFER(histogram);
    DEFINE_SHADER_BUFFER(exposure);

    DEFINE_SHADER_ARGS(params, histogram, exposure);
};

class ToneMappingPass {
public:
    struct CreateInfo {
        uint       histogram_bins = HISTOGRAM_BINS;
        TextureRef color_lut      = nullptr;
    };

    struct Params {
        float histogram_low_percentile  = 0.8f;
        float histogram_high_percentile = 0.95f;
        float eye_adaptation_speed_up   = 1.f;
        float eye_adaptation_speed_down = 0.5f;
        float min_adapted_luminance     = 0.02f;
        float max_adapted_luminance     = 0.5f;
        float exposure_bias             = -0.5f;
        float white_point               = 3.f;
        bool  enable_color_lut          = true;
        bool  enable_tone_mapping       = true;
    };

    struct PreparedCommand {
        ToneMappingParams params{};
        uint3             histogram_dispatch_groups{};
        bool              reset_exposure = false;
    };

    struct RecordResources {
        TextureRef source{};
        TextureRef target{};
        TextureRef color_lut{};
    };

public:
    ToneMappingPass(RenderDevice& device, ShaderManager& manager, CreateInfo info);
    void Process(
        CommandList& cmd_list,
        Params       params,
        TextureRef   source_texture,
        TextureRef   target_texture
    );
    bool AddPasses(
        RenderGraph&                 graph,
        const RTGraphFrameResources& graph_resources,
        const RTContext&             rt_ctx,
        Params                       params,
        TextureRef                   source_texture,
        TextureRef                   target_texture
    );
    void CommitAcceptedFrame(float elapsed_time, bool enabled);

private:
    struct RecordingOwner {
        BufferRef               constants{};
        BufferRef               histogram{};
        BufferRef               exposure{};
        ToneMappingPassPipeline tone_mapping{};
        HistogramPipeline       histogram_pipeline{};
        ExposurePipeline        exposure_pipeline{};
    };

    PreparedCommand Prepare(
        Params            params,
        const TextureRef& source_texture
    ) const;
    RecordResources CaptureResources(
        TextureRef source_texture,
        TextureRef target_texture
    ) const;
    static void RecordConstantsUpload(
        CommandList&           cmd_list,
        const RecordingOwner&  owner,
        const PreparedCommand& command
    );
    static void RecordResetHistogram(
        CommandList&          cmd_list,
        const RecordingOwner& owner
    );
    static void RecordResetExposure(
        CommandList&          cmd_list,
        const RecordingOwner& owner
    );
    static void RecordHistogram(
        CommandList&           cmd_list,
        RecordingOwner&        owner,
        const PreparedCommand& command,
        const RecordResources& resources
    );
    static void RecordExposure(
        CommandList&           cmd_list,
        RecordingOwner&        owner
    );
    static void RecordRender(
        CommandList&           cmd_list,
        RecordingOwner&        owner,
        const RecordResources& resources
    );

    float frame_time     = 0;
    uint  frame_idx      = 0;
    uint  color_lut_size = 0;

    TextureRef                 color_lut;
    bool                       initialized          = false;
    bool                       tone_mapping_enabled = false;
    SharedPtr<RecordingOwner> recording_owner;
};
} // namespace Moer::Render::Raytracing
#endif
