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

// 该管线虽未被当前帧流程调用，但仍是可供工具代码使用的 GPU 生成接口。
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

class CopyTextureComputePipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(CopyTextureComputePipeline);
    DEFINE_SHADER_TEX(src_color);
    DEFINE_SHADER_SAMPLER(spl);
    DEFINE_SHADER_TEX(dst_color);

    DEFINE_SHADER_ARGS(src_color, spl, dst_color);
};

using UtilsSampleTexturePipelineCS [[deprecated("Use CopyTextureComputePipeline instead")]] =
    CopyTextureComputePipeline;

class ShaderUtils {
public:
    explicit ShaderUtils(ShaderManager& manager);
    [[deprecated("RenderDevice is no longer required; use ShaderUtils(ShaderManager&) instead")]] ShaderUtils(
        RenderDevice& device,
        ShaderManager& manager
    );

    GenLowDiscrepancyPipeline& GetGenLowDiscrepancyPipeline() {
        return gen_low_discrepancy_pipeline;
    }

    GenerateMipPdfPipeline& GetGenerateMipPdfPipeline() {
        return generate_mip_pdf_pipeline;
    }

    GenerateMipsPipeline& GetGenerateMipsPipeline() {
        return generate_mips_pipeline;
    }

    ShowTexturePipeline& GetShowTexturePipeline() {
        return show_texture_pipeline;
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

    void SampleTextureCS(CommandList& cmd_list, TextureView input_texture, TextureView output_texture);

    [[deprecated("Output format is provided by the output texture")]] void SampleTextureCS(
        CommandList& cmd_list,
        TextureView  input_texture,
        TextureView  output_texture,
        EPixelFormat /*output_format*/
    ) {
        SampleTextureCS(cmd_list, input_texture, output_texture);
    }

private:
    GenLowDiscrepancyPipeline  gen_low_discrepancy_pipeline;
    GenerateMipPdfPipeline     generate_mip_pdf_pipeline;
    GenerateMipsPipeline       generate_mips_pipeline;
    ShowTexturePipeline        show_texture_pipeline;
    CopyTextureComputePipeline copy_texture_pipeline;
};
} // namespace Moer::Render::Raytracing
#endif
