#include "ShaderUtils.h"
#include "rhi/RHIResource.h"
#include "shader/ShaderResourceManager.h"
#include <type_traits>

namespace Moer::Render {
    inline static uint DivCeil(uint _a, uint _b) {
        return (_a + _b - 1) / _b;
    }
    ShaderUtils::ShaderUtils(RenderDevice& _device, ShaderManager& _manager)
        : manager(_manager), device(_device) {
        gen_low_discrepancy_pipeline = std::move(manager.Compute<GenLowDiscrepancyPipeline>("utils/GenLowDiscrepancySequence.hlsl"));
        generate_mip_pdf_pipeline    = std::move(manager.Compute<GenerateMipPdfPipeline>("lighting/ProcessEnvironmentMap.hlsl"));
        generate_mips_pipeline       = std::move(manager.Compute<GenerateMipsPipeline>("utils/BuildMips.hlsl"));
    }

    void ShaderUtils::GenerateLowDiscrepancySequence(CommandList& _cmd_list, GenLowDiscrepancySequenceParam _param, BufferView _output) {
        assert(_param.num_dimensions == 2);
        assert(_param.num_samples * _param.num_dimensions * sizeof(uint) <= _output.GetByteSize());
        _cmd_list.Compute(gen_low_discrepancy_pipeline, _param, _output).Dispatch(uint3(DivCeil(_param.num_samples, 256), 1, 1), "GenerateLowDiscrepancySequence");
    }

    void ShaderUtils::GenerateMipPdf(CommandList& _cmd_list, const TextureView& _env_map, std::span<TextureView> _integrated_mips) {
        using T = std::remove_const_t<std::remove_reference_t<const TextureView&>>;
        PreprocessEnvironmentMapParams param;
        uint                           width  = _env_map.extent.x;
        uint                           height = _env_map.extent.y;
        //5 mips in one dispatch
        for (uint i = 0; i < _integrated_mips.size(); i += 5) {
            param.src_mip_level  = i;
            param.num_mip_levels = _integrated_mips.size();
            param.src_size       = uint2(width, height);

            _cmd_list.Compute(generate_mip_pdf_pipeline, _env_map, _integrated_mips, param).Dispatch(uint3(DivCeil(width, 32), DivCeil(height, 32), 1), "GenerateMipPdf");
            width  = std::max(1u, width >> 5);
            height = std::max(1u, height >> 5);
        }
    }

    void ShaderUtils::GenerateMips(CommandList& _cmd_list, std::span<TextureView> _mips) {
        BuildMipsParam param;

        uint width  = _mips[0].extent.x;
        uint height = _mips[0].extent.y;
        for (uint i = 0; i < _mips.size(); i += 5) {
            param.num_mip_levels = _mips.size();
            param.src_mip_level  = i;
            param.src_size       = uint2(width, height);
            _cmd_list.Compute(generate_mips_pipeline, _mips, param).Dispatch(uint3(DivCeil(width, 32), DivCeil(height, 32), 1), "GenerateMips");
            width  = std::max(1u, width >> 5);
            height = std::max(1u, height >> 5);
        }
    }
}// namespace Moer::Render