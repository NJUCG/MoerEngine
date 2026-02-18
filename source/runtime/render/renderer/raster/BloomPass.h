#pragma once

#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/raster/post_process/ShaderParameters.h"

#include "RasterConfig.h"
#include "RasterResource.h"

namespace Moer::Render::Raster {

class BloomPassPrefilterPipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(BloomPassPrefilterPipeline);
    DEFINE_SHADER_TEX(input_tex);
    DEFINE_SHADER_SAMPLER(linear_sampler);
    DEFINE_SHADER_CONSTANT_STRUCT(BloomPrefilterParam, param);
    DEFINE_SHADER_ARGS(input_tex, linear_sampler, param);
};

class BloomPassUpSamplePipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(BloomPassUpSamplePipeline);
    DEFINE_SHADER_TEX(upsample_tex);
    DEFINE_SHADER_TEX(downsample_tex);
    DEFINE_SHADER_SAMPLER(linear_sampler);
    DEFINE_SHADER_CONSTANT_STRUCT(BloomUpsampleParam, param);
    DEFINE_SHADER_ARGS(upsample_tex, downsample_tex, linear_sampler, param);
};

class BloomPassDownSamplePipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(BloomPassDownSamplePipeline);
    DEFINE_SHADER_TEX(src_tex);
    DEFINE_SHADER_SAMPLER(linear_sampler);
    DEFINE_SHADER_CONSTANT_STRUCT(BloomDownsampleParam, param);
    DEFINE_SHADER_ARGS(src_tex, linear_sampler, param);
};

class BloomApplyPipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(BloomApplyPipeline);
    DEFINE_SHADER_TEX(bloom_tex);
    DEFINE_SHADER_SAMPLER(linear_sampler);
    DEFINE_SHADER_CONSTANT_STRUCT(BloomApplyParam, param);
    DEFINE_SHADER_ARGS(bloom_tex, linear_sampler, param);
};

class BloomPass {
public:
    BloomPass(RasterContext& context);

    TextureWithHandle
    Process(RasterContext& context, const RasterConfig& ui_config, TextureWithHandle& input_texture);

private:
    BloomPassPrefilterPipeline  prefilter_pipeline;
    BloomPassUpSamplePipeline   upsample_pipeline;
    BloomPassDownSamplePipeline downsample_pipeline;
    BloomApplyPipeline          apply_pipeline;
};
} // namespace Moer::Render::Raster
