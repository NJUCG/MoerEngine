#include "ShaderUtils.h"
#include "RaytracingGraphResources.h"
#include <stdexcept>
#include <format>

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

RenderGraph::TextureHandle ImportSampledTexture(RenderGraph& graph, const char* name, Texture* texture) {
    const auto handle = ImportRTGraphTexture(graph, name, TextureRef(texture));
    graph.SetInitialState(handle, RenderGraph::TextureState::Sampled, RenderGraph::QueueRole::Graphics, RenderGraph::AccessMode::Read);
    graph.Export(handle, RenderGraph::TextureState::Sampled, RenderGraph::QueueRole::Graphics, RenderGraph::AccessMode::Read);
    return handle;
}

void RecordUtilityGraph(RenderGraph& graph, CommandList& commands) {
    commands.SetResourceStateOwnership(ERHIResourceStateOwnership::Compiled);
    if (!graph.Compile() || !graph.RecordAndMergeFrontendCommands(commands, false)) {
        throw std::runtime_error("Raytracing utility graph: " + graph.GetCompileError());
    }
}

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

    RenderGraph graph("RT.NeighborOffsets", RenderGraph::QueueTopology::FromRHI());
    const auto output = ImportRTGraphBuffer(graph, "Offsets", BufferRef(_output.GetBuffer()));
    graph.SetInitialState(output, RenderGraph::BufferState::ShaderResource, RenderGraph::QueueRole::Graphics, RenderGraph::AccessMode::Read);
    graph.Export(output, RenderGraph::BufferState::ShaderResource, RenderGraph::QueueRole::Graphics, RenderGraph::AccessMode::Read);
    graph.AddRecordPass("Upload", [=](RenderGraph::PassBuilder& builder) {
        builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::PipelineType::Copy)
            .Write(output, RenderGraph::BufferState::TransferDestination);
    }, [data = std::move(data), _output](CommandList& commands) {
        commands.CopyFrom(std::span<const byte>(reinterpret_cast<const byte*>(data.data()), data.size()), _output);
    });
    RecordUtilityGraph(graph, _cmd_list);
}

void ShaderUtils::GenerateMipPdf(
    CommandList&           _cmd_list,
    const TextureView&     _env_map,
    std::span<TextureView> _integrated_mips
) {
    if (_integrated_mips.empty()) {
        return;
    }

    RenderGraph graph("RT.EnvironmentPdf", RenderGraph::QueueTopology::FromRHI());
    const auto input = ImportSampledTexture(graph, "Environment", _env_map.GetTexture());
    const auto output = ImportSampledTexture(graph, "Pdf", _integrated_mips.front().GetTexture());
    uint width = _env_map.extent.x, height = _env_map.extent.y;
    for (uint i = 0; i < _integrated_mips.size(); i += 5) {
        PreprocessEnvironmentMapParams param{};
        param.src_mip_level = i;
        param.num_mip_levels = _integrated_mips.size();
        param.src_size = uint2(width, height);
        graph.AddRecordPass(std::format("Integrate.{}", i), [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::PipelineType::Compute)
                .Read(input, RenderGraph::TextureState::Sampled)
                .ReadWrite(output, RenderGraph::TextureState::UnorderedAccess);
        }, [=, this](CommandList& commands) {
            commands.Compute(generate_mip_pdf_pipeline, _env_map, _integrated_mips, param)
                .Dispatch(uint3(DivCeil(width, 32), DivCeil(height, 32), 1), "GenerateMipPdf");
        });
        width = std::max(1u, width >> 5);
        height = std::max(1u, height >> 5);
    }
    RecordUtilityGraph(graph, _cmd_list);
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

void ShaderUtils::GenerateMips(CommandList& commands, std::span<TextureView> mips) {
    if (mips.size() < 2) { return; }
    RenderGraph graph("RT.GenerateMips", RenderGraph::QueueTopology::FromRHI());
    const auto texture = ImportSampledTexture(graph, "Image", mips.front().GetTexture());
    for (uint i = 0; i + 1 < mips.size(); i += 5) {
        const auto chunk = mips.subspan(i, std::min<size_t>(6, mips.size() - i));
        graph.AddRecordPass(std::format("Mips.{}", i), [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::PipelineType::Compute)
                .Read(texture, RenderGraph::TextureState::UnorderedAccess, RenderGraph::TextureRange::Mips(i, 1))
                .Write(texture, RenderGraph::TextureState::UnorderedAccess, RenderGraph::TextureRange::Mips(i + 1, chunk.size() - 1));
        }, [=, this](CommandList& list) { GenerateMipsChunk(list, chunk); });
    }
    RecordUtilityGraph(graph, commands);
}

void ShaderUtils::SampleTextureCS(
    CommandList& cmd_list,
    TextureView  input_texture,
    TextureView  output_texture
) {
    RenderGraph graph("RT.SampleTexture", RenderGraph::QueueTopology::FromRHI());
    const auto input = ImportSampledTexture(graph, "Input", input_texture.GetTexture());
    const auto output = ImportSampledTexture(graph, "Output", output_texture.GetTexture());
    graph.AddRecordPass("Sample", [=](RenderGraph::PassBuilder& builder) {
        builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::PipelineType::Compute)
            .Read(input, RenderGraph::TextureState::Sampled)
            .Write(output, RenderGraph::TextureState::UnorderedAccess, RenderGraph::TextureRange::Mips(output_texture.mip_level, output_texture.num_mips));
    }, [=, this](CommandList& commands) {
        Sampler linear_clamp_sampler{SF_LINEAR, SAM_CLAMP_TO_EDGE};
        commands.Compute(copy_texture_pipeline, input_texture, linear_clamp_sampler, output_texture)
            .Dispatch(uint3(DivCeil(output_texture.extent.x, 16), DivCeil(output_texture.extent.y, 16), 1), "SampleTextureCS");
    });
    RecordUtilityGraph(graph, cmd_list);
}

} // namespace Moer::Render::Raytracing
