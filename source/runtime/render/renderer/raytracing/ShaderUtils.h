#ifndef MOER_SHADER_UTILS_H
#define MOER_SHADER_UTILS_H

// Raytracing 渲染器共用的计算与全屏辅助函数。

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

class CopyTextureComputePipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(CopyTextureComputePipeline);
    DEFINE_SHADER_TEX(src_color);
    DEFINE_SHADER_SAMPLER(spl);
    DEFINE_SHADER_TEX(dst_color);

    DEFINE_SHADER_ARGS(src_color, spl, dst_color);
};

class ShaderUtils {
public:
    explicit ShaderUtils(ShaderManager& manager);

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

    void SampleTextureCS(CommandList& cmd_list, TextureView input_texture, TextureView output_texture);

private:
    GenerateMipPdfPipeline     generate_mip_pdf_pipeline;
    GenerateMipsPipeline       generate_mips_pipeline;
    ShowTexturePipeline        show_texture_pipeline;
    CopyTextureComputePipeline copy_texture_pipeline;
};
} // namespace Moer::Render::Raytracing
#endif
