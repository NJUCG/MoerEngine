#ifndef MOER_SHADER_UTILS_H
#define MOER_SHADER_UTILS_H
#include "rhi/RHI.h"
#include <shader/ShaderPipeline.h>
#include <shaderheaders/shared/utils/ShaderParameters.h>

namespace Moer::Render {
    class ShaderManager;
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

    class ShaderUtils {
    public:
        ShaderUtils(RenderDevice& _device, ShaderManager& _manager);

        GenLowDiscrepancyPipeline& GetGenLowDiscrepancyPipeline() { return gen_low_discrepancy_pipeline; }
        GenerateMipPdfPipeline&    GetGenerateMipPdfPipeline() { return generate_mip_pdf_pipeline; }
        GenerateMipsPipeline&      GetGenerateMipsPipeline() { return generate_mips_pipeline; }

        void GenerateLowDiscrepancySequence(CommandList& _cmd_list, GenLowDiscrepancySequenceParam _param, BufferView _output);
        void GenerateMipPdf(CommandList& _cmd_list, const TextureView& _env_map, std::span<TextureView> _integrated_mips);
        void GenerateMips(CommandList& _cmd_list, std::span<TextureView> _mips);

    private:
        GenLowDiscrepancyPipeline gen_low_discrepancy_pipeline;
        GenerateMipPdfPipeline    generate_mip_pdf_pipeline;
        GenerateMipsPipeline      generate_mips_pipeline;

        ShaderManager& manager;
        RenderDevice&  device;
    };
}// namespace Moer::Render
#endif