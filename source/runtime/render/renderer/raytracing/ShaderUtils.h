#ifndef MOER_SHADER_UTILS_H
#define MOER_SHADER_UTILS_H

#include "PixelFormat.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHIResource.h"
#include "shaderheaders/shared/ShaderParameters.h"
#include <shader/ShaderPipeline.h>
#include <shaderheaders/shared/utils/ShaderParameters.h>

namespace Moer::Render {
class ShaderManager;
}

namespace Moer::Render::Raytracing {

static constexpr uint s_max_mip_levels = 14;

class GenLowDiscrepancyPipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(GenLowDiscrepancyPipeline);

    DEFINE_SHADER_CONSTANT_STRUCT(GenLowDiscrepancySequenceParam, param);
    DEFINE_SHADER_BUFFER(output);

    DEFINE_SHADER_ARGS(param, output);
};

struct GenerateMipPdfPipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(GenerateMipPdfPipeline);
    DEFINE_SHADER_TEX(env_map);
    DEFINE_SHADER_TEX_ARRAY(integrated_mips, s_max_mip_levels);
    DEFINE_SHADER_CONSTANT_STRUCT(PreprocessEnvironmentMapParams, param);

    DEFINE_SHADER_ARGS(env_map, integrated_mips, param);
};

struct GenerateMipsPipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(GenerateMipsPipeline);
    DEFINE_SHADER_TEX_ARRAY(mips, s_max_mip_levels);
    DEFINE_SHADER_CONSTANT_STRUCT(BuildMipsParam, param);

    DEFINE_SHADER_ARGS(mips, param);
};

struct ShowTexturePipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(ShowTexturePipeline);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_TEX(src_tex);
    DEFINE_SHADER_CONSTANT_STRUCT(ShowTextureParams, param);

    DEFINE_SHADER_ARGS(param, src_tex, bdls);
};

class UtilsSampleTexturePipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(UtilsSampleTexturePipeline);
    DEFINE_SHADER_TEX(src_color);
    DEFINE_SHADER_SAMPLER(spl);

    DEFINE_SHADER_ARGS(src_color, spl);
};

class UtilsSampleTexturePipelineCS : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(UtilsSampleTexturePipelineCS);
    DEFINE_SHADER_TEX(src_color);
    DEFINE_SHADER_SAMPLER(spl);
    DEFINE_SHADER_TEX(dst_color);

    DEFINE_SHADER_ARGS(src_color, spl, dst_color);
};

class ShaderUtils {
public:
    ShaderUtils(RenderDevice& _device, ShaderManager& _manager);

    GenLowDiscrepancyPipeline& GetGenLowDiscrepancyPipeline() {
        return gen_low_discrepancy_pipeline;
    }
    GenerateMipPdfPipeline& GetGenerateMipPdfPipeline() {
        return generate_mip_pdf_pipeline;
    }
    GenerateMipsPipeline& GetGenerateMipsPipeline() {
        return generate_mips_pipeline;
    }
    ShowTexturePipeline& GetShowTexturePipeline(EPixelFormat format) {
        return show_texture_pipeline_map.at(format);
    }

    void GenerateLowDiscrepancySequence(
        CommandList&                   _cmd_list,
        GenLowDiscrepancySequenceParam _param,
        BufferView                     _output
    );
    void GenerateMipPdf(
        CommandList&           _cmd_list,
        const TextureView&     _env_map,
        std::span<TextureView> _integrated_mips
    );
    void GenerateMips(CommandList& _cmd_list, std::span<TextureView> _mips);
    void ShowTexture(
        CommandList&             _cmd_list,
        BindlessArrayRef         _bdls,
        const ShowTextureParams& _param,
        TextureRef               _src_tex,
        TextureRef               _dst_texture
    );

    void SampleTextureRaster(
        CommandList& _cmd_list,
        TextureView  _input_texture,
        TextureView  _output_texture,
        EPixelFormat _output_format
    );

    void SampleTextureCS(
        CommandList& _cmd_list,
        TextureView  _input_texture,
        TextureView  _output_texture,
        EPixelFormat _output_format
    );

private:
    GenLowDiscrepancyPipeline    gen_low_discrepancy_pipeline;
    GenerateMipPdfPipeline       generate_mip_pdf_pipeline;
    GenerateMipsPipeline         generate_mips_pipeline;
    UnorderedMap<EPixelFormat, ShowTexturePipeline> show_texture_pipeline_map;
    UtilsSampleTexturePipelineCS sample_texture_cs_pipeline;

    UnorderedMap<EPixelFormat, UtilsSampleTexturePipeline> sample_texture_pipeline_map;

    ShaderManager& manager;
    RenderDevice&  device;
};
} // namespace Moer::Render::Raytracing
#endif