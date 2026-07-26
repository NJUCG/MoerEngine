#include "ShaderUtils.h"

// 实现渲染器侧工具调度和 CPU 生成的采样数据。

#include "PixelFormat.h"
#include "log/LogSystem.h"
#include "rhi/RHIResource.h"
#include "shader/ShaderResourceManager.h"
#include "shaderheaders/shared/utils/Packing.h"

#include <algorithm>

namespace Moer::Render::Raytracing {

namespace {

constexpr uint DivCeil(uint value, uint divisor) {
    return (value + divisor - 1) / divisor;
}

constexpr uint MipExtent(uint base_extent, uint mip_level) {
    return std::max(1u, base_extent >> std::min(mip_level, 31u));
}

static_assert(MipExtent(4096, 0) == 4096);
static_assert(MipExtent(4096, 5) == 128);
static_assert(MipExtent(4096, 10) == 4);
static_assert(MipExtent(4096, 12) == 1);
static_assert(MipExtent(1, 13) == 1);

} // namespace

ShaderUtils::ShaderUtils(ShaderManager& manager) {
    gen_low_discrepancy_pipeline =
        std::move(manager.Compute<GenLowDiscrepancyPipeline>("core/utils/GenLowDiscrepancySequence.hlsl"));
    generate_mip_pdf_pipeline = std::move(manager.Compute<GenerateMipPdfPipeline>(
        "pipelines/raytracing/lighting/precompute/ProcessEnvironmentMap.hlsl"
    ));
    generate_mips_pipeline    = std::move(manager.Compute<GenerateMipsPipeline>("core/utils/BuildMips.hlsl"));

    copy_texture_pipeline =
        std::move(manager.Compute<CopyTextureComputePipeline>("core/utils/CopyTexture.cs.hlsl"));
}

ShaderUtils::ShaderUtils(RenderDevice& /*device*/, ShaderManager& manager) : ShaderUtils(manager) {}

void ShaderUtils::GenerateLowDiscrepancySequence(
    CommandList&                   _cmd_list,
    GenLowDiscrepancySequenceParam _param,
    BufferView                     _output
) {
    assert(_param.num_dimensions == 2);
    assert(_param.num_samples * _param.num_dimensions <= _output.GetByteSize());
    Array<int8>   data(_param.num_samples * 2);
    constexpr int radius_scale = 250;
    const float   phi2         = 1.0f / 1.3247179572447f;
    uint32_t      num          = 0;
    float         u            = 0.5f;
    float         v            = 0.5f;
    while (num < _param.num_samples * 2) {
        u += phi2;
        v += phi2 * phi2;
        if (u >= 1.0f) {
            u -= 1.0f;
        }
        if (v >= 1.0f) {
            v -= 1.0f;
        }

        const float radius_squared = (u - 0.5f) * (u - 0.5f) + (v - 0.5f) * (v - 0.5f);
        if (radius_squared > 0.25f) {
            continue;
        }

        data[num++] = static_cast<int8>((u - 0.5f) * radius_scale);
        data[num++] = static_cast<int8>((v - 0.5f) * radius_scale);
    }

    _cmd_list.CopyFrom(std::span<byte>((byte*)data.data(), data.size()), _output);

    _cmd_list.AddCallback([data(std::move(data))]() {});
}

void ShaderUtils::GenerateMipPdf(
    CommandList&           _cmd_list,
    const TextureView&     _env_map,
    std::span<TextureView> _integrated_mips
) {
    if (_integrated_mips.empty()) {
        return;
    }

    PreprocessEnvironmentMapParams param;
    uint                           width  = _env_map.extent.x;
    uint                           height = _env_map.extent.y;
    // 每次调度最多处理连续五级 mip。
    for (uint i = 0; i < _integrated_mips.size(); i += 5) {
        param.src_mip_level  = i;
        param.num_mip_levels = _integrated_mips.size();
        param.src_size       = uint2(width, height);

        _cmd_list.Compute(generate_mip_pdf_pipeline, _env_map, _integrated_mips, param)
            .Dispatch(uint3(DivCeil(width, 32), DivCeil(height, 32), 1), "GenerateMipPdf");
        width  = std::max(1u, width >> 5);
        height = std::max(1u, height >> 5);
    }
}

void ShaderUtils::GenerateMipsChunk(CommandList& cmd_list, std::span<TextureView> mips) {
    if (mips.size() < 2) {
        return;
    }

    assert(mips.size() <= 6);
    const TextureView& source_mip = mips.front();
    const uint         width      = MipExtent(source_mip.extent.x, source_mip.mip_level);
    const uint         height     = MipExtent(source_mip.extent.y, source_mip.mip_level);
    BuildMipsParam     param{};
    param.num_mip_levels = static_cast<uint>(mips.size());
    param.src_mip_level  = 0;
    param.src_size       = uint2(width, height);
    cmd_list.Compute(generate_mips_pipeline, mips, param)
        .Dispatch(uint3(DivCeil(width, 32), DivCeil(height, 32), 1), "GenerateMips");
}

void ShaderUtils::GenerateMips(CommandList& _cmd_list, std::span<TextureView> _mips) {
    if (_mips.empty()) {
        return;
    }

    BuildMipsParam param;

    uint width  = _mips[0].extent.x;
    uint height = _mips[0].extent.y;

    if (_mips.size() > 5) {
        /**
         * FIXME:
         * 注意到，这里每次都会将所有mips全部传入RHI中，所以如果mips > 5的话，那么每个mip都会被重复传入RHI中
         * 目前不确定这里是否会引发bug，需要进一步测试
         * 
         * 确认后/修复后，请删除本段代码
         */
        static bool b_first_time = true; // 只警告一次
        if (b_first_time) {
            b_first_time = false;
            LOG_WARNING(
                "Here may be a bug when total mips > 5. GenerateMips for {} will be called. mip_level={}, "
                "extent={},{}",
                _mips[0].GetTexture()->GetName(),
                _mips[0].mip_level,
                _mips[0].extent.x,
                _mips[0].extent.y
            );
        }
    }

    for (uint i = 0; i < _mips.size(); i += 5) {
        param.num_mip_levels = _mips.size();
        param.src_mip_level  = i;
        param.src_size       = uint2(width, height);
        _cmd_list.Compute(generate_mips_pipeline, _mips, param)
            .Dispatch(uint3(DivCeil(width, 32), DivCeil(height, 32), 1), "GenerateMips");
        width  = std::max(1u, width >> 5);
        height = std::max(1u, height >> 5);
    }
}

void ShaderUtils::SampleTextureCS(
    CommandList& cmd_list,
    TextureView  input_texture,
    TextureView  output_texture
) {
    Sampler linear_clamp_sampler{SF_LINEAR, SAM_CLAMP_TO_EDGE};
    cmd_list.Compute(copy_texture_pipeline, input_texture, linear_clamp_sampler, output_texture)
        .Dispatch(
            uint3(DivCeil(output_texture.extent.x, 16), DivCeil(output_texture.extent.y, 16), 1),
            "SampleTextureCS"
        );
}

} // namespace Moer::Render::Raytracing
