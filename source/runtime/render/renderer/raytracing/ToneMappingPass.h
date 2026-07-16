#ifndef MOER_TONE_MAPPING_PASS_H
#define MOER_TONE_MAPPING_PASS_H

// 计算亮度曝光，并将 HDR 光照映射到显示目标。

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

public:
    ToneMappingPass(RenderDevice& device, ShaderManager& manager, CreateInfo info);
    void Process(
        CommandList& cmd_list,
        Params       params,
        TextureRef   source_texture,
        TextureRef   target_texture
    );
    void AdvanceFrame(float _frame_time);

private:
    void ComputeExposure(CommandList& _cmd_list);
    void ComputeHistogram(CommandList& _cmd_list, TextureRef _src_tex);
    void ResetHistogram(CommandList& _cmd_list);
    void ResetExposure(CommandList& _cmd_list);
    void Render(CommandList& _cmd_list, TextureRef _src_tex, TextureRef _target);

    float frame_time     = 0;
    uint  frame_idx      = 0;
    uint  color_lut_size = 0;

    BufferRef   tone_mapping_constants;
    BufferRef   histogram_buffer;
    BufferRef   exposure_buffer;
    TextureRef  color_lut;
    Array<byte> upload_data;
    bool        tone_mapping_enabled = false;

    ToneMappingPassPipeline tone_mapping_pass_pipeline;
    HistogramPipeline       histogram_pipeline;
    ExposurePipeline        exposure_pipeline;

};
} // namespace Moer::Render::Raytracing
#endif
