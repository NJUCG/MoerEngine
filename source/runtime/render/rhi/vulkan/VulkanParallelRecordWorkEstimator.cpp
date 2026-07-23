#include "VulkanParallelRecordWorkEstimator.h"

#include "misc/Traits.h"
#include "rhi/RHIImpl.h"

#include <algorithm>
#include <limits>
#include <variant>

namespace Moer::Render::VulkanParallelRecordDetail {
namespace {

uint64 EstimateShaderArgumentRecordWorkUnits(
    const PipelineHandle& _pipeline,
    const ArrayArguments& _arguments
) {
    uint64       work_units     = 0;
    const size_t argument_count = std::min<size_t>(_arguments.args.size(), 64);
    for (size_t index = 0; index < argument_count; ++index) {
        if ((_pipeline.valid_bits & (uint64{1} << index)) == 0) {
            continue;
        }
        work_units += std::visit(
            Overload{
                [](TInvalidArg) -> uint64 { return 0; },
                [](const TextureViewArray& _views) -> uint64 {
                    return std::max<size_t>(1, _views.size());
                },
                [](const BufferViewArray& _views) -> uint64 {
                    return std::max<size_t>(1, _views.size());
                },
                [](const auto&) -> uint64 { return 1; },
            },
            _arguments.args[index]
        );
    }
    return work_units;
}

const ArrayArguments* ResolveArguments(
    const TShaderArgArray& _arguments,
    const TCachedArgArray& _cached_arguments
) {
    if (std::holds_alternative<ArrayArguments>(_arguments)) {
        return &std::get<ArrayArguments>(_arguments);
    }
    if (std::holds_alternative<ArrayArgReference>(_arguments)) {
        const uint32 index = std::get<ArrayArgReference>(_arguments)();
        return index < _cached_arguments.size() ? &_cached_arguments[index] : nullptr;
    }
    return nullptr;
}

uint64 EstimateMeshDrawRecordWorkUnits(const MeshDrawData& _draw_data) {
    uint64 work_units = _draw_data.vtx_views.empty() ? 0u : 1u;
    work_units += std::holds_alternative<IndexBuffer>(_draw_data.idx_view) ? 1u : 0u;
    // Direct draws each emit one vkCmdDraw* call. An indirect payload is one
    // recording operation regardless of its GPU-side draw count.
    work_units += _draw_data.draw_params.size();
    work_units += _draw_data.indirect_draw_param.has_value() ? 1u : 0u;
    return work_units;
}

} // namespace

uint32 EstimateWorkUnits(
    const Command& _command, const TCachedArgArray& _cached_arguments
) {
    uint64 work_units = 1;
    switch (_command.Type()) {
        case Command::EType::SetDrawState: {
            const auto& draw = static_cast<const SetDrawStateCmd&>(_command);
            // Label pair, rendering pair, PSO, descriptor bind, viewport and scissor.
            work_units = 8 + EstimateShaderArgumentRecordWorkUnits(
                                 draw.Pipeline(), draw.Args()
                             );
            work_units += draw.Args().constants.empty() ? 0u : 1u;
            for (const MeshDrawData& mesh : draw.DrawData()) {
                work_units += EstimateMeshDrawRecordWorkUnits(mesh);
            }
            break;
        }
        case Command::EType::MultiDraw: {
            const auto& draw = static_cast<const MultiDrawCmd&>(_command);
            // Label pair, rendering pair, viewport and scissor.
            work_units = 6;
            for (const DrawBatchElement& element : draw.draw_batch.draw_cmds) {
                // PSO plus descriptor bind, followed by payload-dependent work.
                work_units += 2;
                const ArrayArguments* arguments = ResolveArguments(
                    element.args, _cached_arguments
                );
                if (arguments != nullptr) {
                    work_units += EstimateShaderArgumentRecordWorkUnits(
                        element.handle, *arguments
                    );
                    work_units += arguments->constants.empty() ? 0u : 1u;
                }
                std::visit(
                    Overload{
                        [&](const Array<MeshDrawData>& meshes) {
                            for (const MeshDrawData& mesh : meshes) {
                                work_units += EstimateMeshDrawRecordWorkUnits(mesh);
                            }
                        },
                        [&](const Array<DispatchMeshData>& dispatches) {
                            work_units += dispatches.size();
                        },
                    },
                    element.mesh_dispatch_data
                );
            }
            break;
        }
        case Command::EType::ShaderDispatch: {
            const auto& dispatch  = static_cast<const DispatchCmd&>(_command);
            const auto& arguments = dispatch.Args(_cached_arguments);
            // Label pair, PSO, descriptor bind and one direct/indirect dispatch.
            work_units = 5 + EstimateShaderArgumentRecordWorkUnits(
                                 dispatch.Pipeline(), arguments
                             );
            work_units += arguments.constants.empty() ? 0u : 1u;
            break;
        }
        case Command::EType::UploadBuffer:
        case Command::EType::UploadTexture:
        case Command::EType::CopyBackBuffer:
        case Command::EType::CopyBackTexture:
        case Command::EType::BufferToBuffer:
        case Command::EType::BufferToTexture:
        case Command::EType::TextureToBuffer:
        case Command::EType::TextureToTexture:
        case Command::EType::ClearResource:
            // Marker pair plus one copy/clear call. Bytes and extents are GPU work.
            work_units = 3;
            break;
        default:
            // Serial-only commands never reach a worker, but diagnostics stay total.
            work_units = 1;
            break;
    }
    return static_cast<uint32>(std::min<uint64>(
        std::max<uint64>(1, work_units), std::numeric_limits<uint32>::max()
    ));
}

} // namespace Moer::Render::VulkanParallelRecordDetail
