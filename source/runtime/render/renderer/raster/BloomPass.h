#pragma once

#include "math/Function.h"
#include "misc/MMemory.h"
#include "scene/Camera.h"
#include "scene/Material.h"
#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/raster/post_process/ShaderParameters.h"

#include "RasterConfig.h"
#include "RasterResource.h"
#include "RasterTool.h"

namespace Moer::Render::Raster {

class BloomPassPrefilterPipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(BloomPassPrefilterPipeline);
    DEFINE_SHADER_CONSTANT_STRUCT(BloomPrefilterBindlessParam, param);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_ARGS(bdls, param);
};

class BloomPassUpSamplePipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(BloomPassUpSamplePipeline);
    DEFINE_SHADER_CONSTANT_STRUCT(BloomUpsampleBindlessParam, param);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_ARGS(bdls, param);
};

class BloomPassDownSamplePipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(BloomPassDownSamplePipeline);
    DEFINE_SHADER_CONSTANT_STRUCT(BloomDownsampleBindlessParam, param);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_ARGS(bdls, param);
};

class BloomApplyPipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(BloomApplyPipeline);
    DEFINE_SHADER_CONSTANT_STRUCT(BloomApplyBindlessParam, param);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_ARGS(bdls, param);
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