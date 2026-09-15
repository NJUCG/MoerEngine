// 提取高亮区域、构建 Bloom 金字塔，并合成回场景颜色。
#pragma once

#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/raster/post_process/ShaderParameters.h"

#include "RasterConfig.h"
#include "RasterResource.h"
#include "rendergraph/RenderGraph.h"

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
    struct GraphResources {
        RenderGraph::TextureHandle input{};
        RenderGraph::TokenHandle   bloom_chain{};
    };

    struct DownsampleDispatch {
        TextureView          source{};
        Rect2D               render_area{};
        BloomDownsampleParam shader{};
        uint32                mip{0};
    };

    struct UpsampleDispatch {
        TextureView        source{};
        TextureView        downsample{};
        Rect2D             render_area{};
        BloomUpsampleParam shader{};
        uint32             mip{0};
    };

    struct RecordParameters {
        bool                      enabled{false};
        TextureWithHandle         input{};
        TextureWithHandle         downsample_chain{};
        TextureWithHandle         upsample_chain{};
        Sampler                   linear_sampler{SF_LINEAR, SAM_CLAMP_TO_EDGE};
        BloomPrefilterParam       prefilter{};
        BloomApplyParam           apply{};
        Array<DownsampleDispatch> downsample_dispatches{};
        Array<UpsampleDispatch>   upsample_dispatches{};
    };

    BloomPass(RasterContext& context);

    [[nodiscard]] RecordParameters Prepare(
        const RasterContext& context,
        const RasterConfig&  raster_config,
        TextureWithHandle    input_texture
    ) const;
    void Record(CommandList& cmd_list, const RecordParameters& parameters);
    RenderGraph::PreparedPassHandle AddToGraph(
        RenderGraph& graph,
        const RasterContext& context,
        const RasterConfig& raster_config,
        TextureWithHandle input_texture,
        GraphResources resources
    );

    TextureWithHandle
    Process(RasterContext& context, const RasterConfig& raster_config, TextureWithHandle input_texture);

private:
    BloomPassPrefilterPipeline  prefilter_pipeline;
    BloomPassUpSamplePipeline   upsample_pipeline;
    BloomPassDownSamplePipeline downsample_pipeline;
    BloomApplyPipeline          apply_pipeline;
};
} // namespace Moer::Render::Raster
