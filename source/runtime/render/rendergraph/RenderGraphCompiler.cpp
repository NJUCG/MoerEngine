#include "rendergraph/RenderGraphCompiler.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <sstream>
#include <tuple>

namespace Moer::Render {

namespace {

using ResourceKind  = RenderGraph::ResourceKind;
using TextureAspect = RenderGraph::TextureAspect;
using ResourceRange = RenderGraph::ResourceRange;

[[nodiscard]] constexpr uint8_t AspectMask(TextureAspect aspect) {
    return static_cast<uint8_t>(aspect);
}

[[nodiscard]] bool HasAspect(TextureAspect mask, TextureAspect aspect) {
    return (AspectMask(mask) & AspectMask(aspect)) != 0;
}

template<typename T>
void SortUnique(std::vector<T>& values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

[[nodiscard]] auto RangeSortKey(const ResourceRange& range) {
    return std::tuple{
        static_cast<uint8_t>(range.kind),
        AspectMask(range.texture.aspects),
        range.texture.mip_first,
        range.texture.mip_count,
        range.texture.layer_first,
        range.texture.layer_count,
        range.buffer.offset,
        range.buffer.size
    };
}

[[nodiscard]] bool
ReasonLess(const RenderGraph::CompiledEdgeReason& lhs, const RenderGraph::CompiledEdgeReason& rhs) {
    const auto lhs_key =
        std::tuple{static_cast<uint8_t>(lhs.kind), lhs.resource.index, lhs.input_version, lhs.output_version};
    const auto rhs_key =
        std::tuple{static_cast<uint8_t>(rhs.kind), rhs.resource.index, rhs.input_version, rhs.output_version};
    if (lhs_key != rhs_key) {
        return lhs_key < rhs_key;
    }
    return RangeSortKey(lhs.range) < RangeSortKey(rhs.range);
}

[[nodiscard]] RenderGraph::AccessMode ToAccessMode(bool read, bool write) {
    if (read && write) {
        return RenderGraph::AccessMode::ReadWrite;
    }
    return write ? RenderGraph::AccessMode::Write : RenderGraph::AccessMode::Read;
}

[[nodiscard]] bool HasRead(RenderGraph::AccessMode mode) {
    return mode == RenderGraph::AccessMode::Unknown || mode == RenderGraph::AccessMode::Read ||
           mode == RenderGraph::AccessMode::ReadWrite;
}

[[nodiscard]] bool HasWrite(RenderGraph::AccessMode mode) {
    return mode == RenderGraph::AccessMode::Unknown || mode == RenderGraph::AccessMode::Write ||
           mode == RenderGraph::AccessMode::ReadWrite;
}

[[nodiscard]] bool TextureStateSupports(
    RenderGraph::TextureState state,
    RenderGraph::AccessMode   mode
) {
    using State = RenderGraph::TextureState;
    if (mode == RenderGraph::AccessMode::Unknown) {
        return true;
    }
    if (mode == RenderGraph::AccessMode::None) {
        return state == State::Undefined || state == State::Automatic;
    }
    switch (state) {
        case State::Automatic:
            return true;
        case State::Undefined:
            return false;
        case State::TransferSource:
        case State::ShaderResource:
        case State::Sampled:
        case State::DepthStencilRead:
        case State::Present:
            return HasRead(mode) && !HasWrite(mode);
        case State::TransferDestination:
            return mode == RenderGraph::AccessMode::Write;
        case State::RenderTarget:
        case State::DepthStencilWrite:
            return HasWrite(mode);
        case State::UnorderedAccess:
            return HasRead(mode) || HasWrite(mode);
    }
    return false;
}

[[nodiscard]] bool TextureStateSupportsAspects(
    RenderGraph::TextureState state,
    TextureAspect             aspects
) {
    const uint8_t selected = AspectMask(aspects);
    switch (state) {
        case RenderGraph::TextureState::RenderTarget:
        case RenderGraph::TextureState::UnorderedAccess:
        case RenderGraph::TextureState::Present:
            return selected == AspectMask(TextureAspect::Color);
        case RenderGraph::TextureState::DepthStencilRead:
        case RenderGraph::TextureState::DepthStencilWrite: {
            const uint8_t depth_stencil = AspectMask(TextureAspect::Depth) |
                                          AspectMask(TextureAspect::Stencil);
            return selected != 0 && (selected & depth_stencil) == selected;
        }
        default:
            return true;
    }
}

[[nodiscard]] bool BufferStateSupports(
    RenderGraph::BufferState state,
    RenderGraph::AccessMode  mode
) {
    using State = RenderGraph::BufferState;
    if (mode == RenderGraph::AccessMode::Unknown) {
        return true;
    }
    if (mode == RenderGraph::AccessMode::None) {
        return state == State::Undefined || state == State::Automatic;
    }
    switch (state) {
        case State::Automatic:
            return true;
        case State::Undefined:
            return false;
        case State::TransferSource:
        case State::VertexBuffer:
        case State::IndexBuffer:
        case State::IndirectArgument:
        case State::ShaderResource:
        case State::AccelerationStructureBuildInput:
        case State::AccelerationStructureRead:
            return HasRead(mode) && !HasWrite(mode);
        case State::TransferDestination:
        case State::AccelerationStructureWrite:
            return mode == RenderGraph::AccessMode::Write;
        case State::UnorderedAccess:
            return HasRead(mode) || HasWrite(mode);
    }
    return false;
}

[[nodiscard]] bool StatesCompatible(
    const RenderGraph::ResourceState& previous,
    const RenderGraph::ResourceState& next
) {
    if (previous.kind != next.kind || previous.IsAutomatic() || next.IsAutomatic()) {
        return false;
    }
    if (previous == next) {
        return true;
    }
    return false;
}

[[nodiscard]] RenderGraph::ResourceState CanonicalCompatibleState(
    const RenderGraph::ResourceState& lhs,
    const RenderGraph::ResourceState& rhs
) {
    if (lhs == rhs) {
        return lhs;
    }
    assert(StatesCompatible(lhs, rhs));
    return lhs;
}

template<typename ResourceDeclaration>
[[nodiscard]] bool IsExclusive(const ResourceDeclaration& resource) {
    return resource.kind == ResourceKind::Texture ?
               resource.texture_desc.sharing_mode == RenderGraph::TextureDesc::SharingMode::Exclusive :
           resource.kind == ResourceKind::Buffer ?
               resource.buffer_desc.sharing_mode == RenderGraph::TextureDesc::SharingMode::Exclusive :
               false;
}

void RetainCurrentOwnerFamilySources(
    std::vector<RenderGraph::CompiledBarrierSource>& sources,
    const RenderGraph::QueueTopology&                topology,
    RenderGraph::QueueRole                           owner_queue
) {
    assert(owner_queue != RenderGraph::QueueRole::None);
    const uint32_t owner_family = topology.Resolve(owner_queue).family_id;
    std::erase_if(sources, [&](const RenderGraph::CompiledBarrierSource& source) {
        return source.domain.queue == RenderGraph::QueueRole::None ||
               topology.Resolve(source.domain.queue).family_id != owner_family;
    });
}

} // namespace

bool RenderGraphCompiler::Compile() {
    graph.compiled = false;
    graph.compile_error.clear();
    graph.compiled_plan.Clear();
    for (auto& resource : graph.resources) {
        resource.first_use = RenderGraph::PassHandle::InvalidIndex;
        resource.last_use  = RenderGraph::PassHandle::InvalidIndex;
    }

    if (!graph.declaration_errors.empty()) {
        return Fail(graph.declaration_errors.front());
    }
    if (graph.passes.empty()) {
        return Fail("graph contains no passes");
    }

    normalized_accesses.assign(graph.passes.size(), {});
    normalized_initial_states.assign(graph.resources.size(), {});
    normalized_final_states.assign(graph.resources.size(), {});
    resource_cells.assign(graph.resources.size(), {});
    resource_version_counts.assign(graph.resources.size(), 0);
    resource_ever_written.assign(graph.resources.size(), false);

    if (!ValidateQueueTopology() || !NormalizeDeclarations() || !BuildAtomicCells() ||
        !BuildSemanticDependencies() || !BuildFinalBarriers() || !BuildTopologicalOrder() ||
        !BuildExecutionOrder()) {
        return false;
    }

    AuditStatePlanCompleteness();
    BuildLifetimes();
    BuildTransientAliasPlan();
    if (!ValidateTransientAliasPlan()) {
        return false;
    }
    BuildQueuePlan();
    BuildDependencyWaves();
    BuildRecordingBatches();
    graph.compiled = true;
    return true;
}

bool RenderGraphCompiler::ValidateQueueTopology() {
    const std::array expected_roles{
        RenderGraph::QueueRole::Graphics,
        RenderGraph::QueueRole::Compute,
        RenderGraph::QueueRole::Copy,
    };
    std::array<RenderGraph::QueueBinding, 3> bindings{};
    for (uint32_t index = 0; index < expected_roles.size(); ++index) {
        bindings[index] = graph.queue_topology.Resolve(expected_roles[index]);
        if (bindings[index].role != expected_roles[index]) {
            return Fail("queue topology contains a binding with the wrong logical role");
        }
    }
    for (uint32_t lhs = 0; lhs < bindings.size(); ++lhs) {
        for (uint32_t rhs = lhs + 1; rhs < bindings.size(); ++rhs) {
            if (bindings[lhs].native_queue_id == bindings[rhs].native_queue_id &&
                bindings[lhs].family_id != bindings[rhs].family_id) {
                return Fail("one native queue id cannot belong to multiple queue families");
            }
        }
    }
    return true;
}

bool RenderGraphCompiler::NormalizeDeclarations() {
    for (uint32_t pass_index = 0; pass_index < graph.passes.size(); ++pass_index) {
        if (!ValidateExecutionDomain(pass_index)) {
            return false;
        }
        const auto& pass = graph.passes[pass_index];
        const bool has_execute = static_cast<bool>(pass.execute);
        const bool has_record  = static_cast<bool>(pass.record);
        if (has_execute == has_record) {
            return Fail(
                "pass '" + pass.name + "' must declare exactly one execute or record callback"
            );
        }
        const bool caller_thread =
            pass.execution_class == RenderGraph::PassExecutionClass::MainThread ||
            pass.execution_class == RenderGraph::PassExecutionClass::CpuPrepare ||
            pass.execution_class == RenderGraph::PassExecutionClass::ExternalControl;
        if (has_execute && !caller_thread) {
            return Fail(
                "main-thread pass '" + pass.name + "' cannot use a command-recording class"
            );
        }
        if (has_record && caller_thread) {
            return Fail(
                "record pass '" + pass.name + "' must use SerialRecord or ParallelRecordEligible"
            );
        }
        if (pass.workload == 0) {
            return Fail("pass '" + pass.name + "' has zero recording workload");
        }
        for (const auto reference : pass.references) {
            if (!graph.IsValidResource(reference)) {
                return Fail("pass '" + pass.name + "' has an invalid resource identity reference");
            }
        }
        for (const auto& access : pass.accesses) {
            if (!graph.IsValidResource(access.resource)) {
                return Fail("pass '" + pass.name + "' references an invalid resource");
            }
            const auto&   resource = graph.resources[access.resource.index];
            if (pass.execution_class == RenderGraph::PassExecutionClass::CpuPrepare &&
                resource.kind != RenderGraph::ResourceKind::Token) {
                return Fail(
                    "cpu-prepare pass '" + pass.name +
                    "' may only access token resources; use Reference for GPU resource identity"
                );
            }
            ResourceRange normalized{};
            if (!NormalizeRange(resource, access.range, normalized) ||
                !ValidateAccessState(
                    pass_index,
                    resource,
                    normalized,
                    access.mode,
                    access.state,
                    access.explicit_state
                )) {
                return false;
            }
            normalized_accesses[pass_index].push_back(
                NormalizedAccess{
                    access.resource,
                    access.mode,
                    normalized,
                    access.state,
                    access.explicit_state
                }
            );
        }
    }

    for (uint32_t resource_index = 0; resource_index < graph.resources.size(); ++resource_index) {
        const auto& resource = graph.resources[resource_index];
        auto normalize_states = [&](
                                    const std::vector<RenderGraph::StateDeclaration>& declarations,
                                    std::vector<NormalizedStateDeclaration>&          normalized_states,
                                    bool                                              initial
                                ) -> bool {
            for (const auto& declaration : declarations) {
                if (declaration.state.IsAutomatic()) {
                    return Fail("boundary state cannot be automatic on resource '" + resource.name + "'");
                }
                if (declaration.queue == RenderGraph::QueueRole::None &&
                    !declaration.state.IsUndefined()) {
                    return Fail("a known boundary state requires an owner queue on resource '" + resource.name + "'");
                }
                if (!initial && declaration.state.IsUndefined()) {
                    return Fail("an exported resource cannot require the undefined state: " + resource.name);
                }
                if (initial && !declaration.state.IsUndefined() &&
                    declaration.boundary_access == RenderGraph::AccessMode::None) {
                    return Fail("a known imported state must describe its previous access on resource '" +
                                resource.name + "'");
                }
                if (initial && declaration.state.IsUndefined() &&
                    declaration.boundary_access != RenderGraph::AccessMode::None) {
                    return Fail("an undefined imported state must have no previous access on resource '" +
                                resource.name + "'");
                }
                if (declaration.boundary_access != RenderGraph::AccessMode::None) {
                    const bool supported = resource.kind == ResourceKind::Texture ?
                                               TextureStateSupports(
                                                   declaration.state.texture,
                                                   declaration.boundary_access
                                               ) :
                                               BufferStateSupports(
                                                   declaration.state.buffer,
                                                   declaration.boundary_access
                                               );
                    if (!supported) {
                        return Fail("boundary access is incompatible with the state on resource '" +
                                    resource.name + "'");
                    }
                }
                ResourceRange normalized{};
                if (!NormalizeRange(resource, declaration.range, normalized)) {
                    return false;
                }
                if (resource.kind == ResourceKind::Texture &&
                    !TextureStateSupportsAspects(
                        declaration.state.texture, normalized.texture.aspects
                    )) {
                    return Fail(
                        "boundary texture state is incompatible with the selected aspects on resource '" +
                        resource.name + "'"
                    );
                }
                normalized_states.push_back(
                    NormalizedStateDeclaration{
                        normalized,
                        declaration.state,
                        declaration.queue,
                        declaration.boundary_access
                    }
                );
            }
            return true;
        };
        if (!normalize_states(
                resource.initial_states, normalized_initial_states[resource_index], true
            ) ||
            !normalize_states(resource.final_states, normalized_final_states[resource_index], false)) {
            return false;
        }
    }
    return true;
}

bool RenderGraphCompiler::NormalizeRange(
    const RenderGraph::ResourceDeclaration& resource,
    const ResourceRange&                    requested,
    ResourceRange&                          normalized
) {
    if (requested.kind != resource.kind) {
        return Fail("resource range kind does not match resource '" + resource.name + "'");
    }

    normalized = requested;
    switch (resource.kind) {
        case ResourceKind::Texture: {
            const auto&   desc          = resource.texture_desc;
            const uint8_t desc_aspects  = AspectMask(desc.aspects);
            const uint8_t known_aspects = AspectMask(TextureAspect::All);
            if (desc.mip_count == 0 || desc.layer_count == 0 || desc_aspects == 0 ||
                (desc_aspects & known_aspects) != desc_aspects) {
                return Fail("texture resource has an invalid descriptor: " + resource.name);
            }

            auto& range = normalized.texture;
            if (range.aspects == TextureAspect::All) {
                range.aspects = desc.aspects;
            }
            const uint8_t requested_aspects = AspectMask(range.aspects);
            const uint8_t available_aspects = AspectMask(desc.aspects);
            if (requested_aspects == 0 || (requested_aspects & available_aspects) != requested_aspects) {
                return Fail("texture range requests unavailable aspects on resource '" + resource.name + "'");
            }
            if (range.mip_first >= desc.mip_count) {
                return Fail("texture range starts beyond the mip count on resource '" + resource.name + "'");
            }
            if (range.mip_count == RenderGraph::RemainingTextureRange) {
                range.mip_count = desc.mip_count - range.mip_first;
            }
            if (range.mip_count == 0 || range.mip_count > desc.mip_count - range.mip_first) {
                return Fail("texture range exceeds the mip count on resource '" + resource.name + "'");
            }
            if (range.layer_first >= desc.layer_count) {
                return Fail(
                    "texture range starts beyond the layer count on resource '" + resource.name + "'"
                );
            }
            if (range.layer_count == RenderGraph::RemainingTextureRange) {
                range.layer_count = desc.layer_count - range.layer_first;
            }
            if (range.layer_count == 0 || range.layer_count > desc.layer_count - range.layer_first) {
                return Fail("texture range exceeds the layer count on resource '" + resource.name + "'");
            }
            break;
        }
        case ResourceKind::Buffer: {
            auto& range = normalized.buffer;
            if (resource.buffer_desc.byte_size == 0) {
                if (range.offset != 0 || range.size != RenderGraph::RemainingBufferRange) {
                    return Fail("an explicitly ranged buffer requires a known byte size: " + resource.name);
                }
                break;
            }
            if (range.offset >= resource.buffer_desc.byte_size) {
                return Fail("buffer range starts beyond the resource size on '" + resource.name + "'");
            }
            if (range.size == RenderGraph::RemainingBufferRange) {
                range.size = resource.buffer_desc.byte_size - range.offset;
            }
            if (range.size == 0 || range.size > resource.buffer_desc.byte_size - range.offset) {
                return Fail("buffer range exceeds the resource size on '" + resource.name + "'");
            }
            break;
        }
        case ResourceKind::Token:
            normalized = ResourceRange::Token();
            break;
    }
    return true;
}

bool RenderGraphCompiler::ValidateExecutionDomain(uint32_t pass_index) {
    const auto& pass   = graph.passes[pass_index];
    const auto  queue  = pass.domain.queue;
    const auto  domain = pass.domain.pipeline;
    if (queue == RenderGraph::QueueRole::None || domain == RenderGraph::PipelineType::None) {
        return Fail("pass '" + pass.name + "' has an incomplete execution domain");
    }
    if (queue == RenderGraph::QueueRole::Copy && domain != RenderGraph::PipelineType::Copy) {
        return Fail("copy queue pass '" + pass.name + "' must use the copy pipeline domain");
    }
    if (domain == RenderGraph::PipelineType::Graphics &&
        queue != RenderGraph::QueueRole::Graphics) {
        return Fail("graphics pipeline pass '" + pass.name + "' must use the graphics queue");
    }
    if (domain == RenderGraph::PipelineType::RayTracing &&
        queue == RenderGraph::QueueRole::Copy) {
        return Fail("ray tracing pass '" + pass.name + "' cannot use the copy queue");
    }
    return true;
}

bool RenderGraphCompiler::ValidateAccessState(
    uint32_t                                pass_index,
    const RenderGraph::ResourceDeclaration& resource,
    const RenderGraph::ResourceRange&       range,
    RenderGraph::AccessMode                 mode,
    const RenderGraph::ResourceState&       state,
    bool                                    explicit_state
) {
    if (resource.kind == ResourceKind::Token) {
        return true;
    }
    if (!explicit_state || state.IsAutomatic()) {
        if (explicit_state) {
            return Fail("pass '" + graph.passes[pass_index].name +
                        "' explicitly requested the automatic state");
        }
        return true;
    }

    const auto& pass = graph.passes[pass_index];
    bool        supported = false;
    bool        transfer_state = false;
    if (resource.kind == ResourceKind::Texture) {
        supported = TextureStateSupports(state.texture, mode) &&
                    state.texture != RenderGraph::TextureState::Undefined &&
                    state.texture != RenderGraph::TextureState::Present;
        transfer_state = state.texture == RenderGraph::TextureState::TransferSource ||
                         state.texture == RenderGraph::TextureState::TransferDestination;
        const bool attachment_state = state.texture == RenderGraph::TextureState::RenderTarget ||
                                      state.texture == RenderGraph::TextureState::DepthStencilRead ||
                                      state.texture == RenderGraph::TextureState::DepthStencilWrite;
        if (attachment_state && pass.domain.pipeline != RenderGraph::PipelineType::Graphics) {
            return Fail("attachment state requires the graphics pipeline in pass '" + pass.name + "'");
        }
        if (!TextureStateSupportsAspects(state.texture, range.texture.aspects)) {
            return Fail(
                "texture state is incompatible with the selected aspects in pass '" + pass.name + "'"
            );
        }
    } else {
        supported     = BufferStateSupports(state.buffer, mode) &&
                    state.buffer != RenderGraph::BufferState::Undefined;
        transfer_state = state.buffer == RenderGraph::BufferState::TransferSource ||
                         state.buffer == RenderGraph::BufferState::TransferDestination;
        const bool graphics_input = state.buffer == RenderGraph::BufferState::VertexBuffer ||
                                    state.buffer == RenderGraph::BufferState::IndexBuffer;
        if (graphics_input && pass.domain.pipeline != RenderGraph::PipelineType::Graphics) {
            return Fail("vertex/index state requires the graphics pipeline in pass '" + pass.name + "'");
        }
    }
    if (!supported) {
        return Fail("access mode is incompatible with the explicit resource state in pass '" +
                    pass.name + "'");
    }
    if ((pass.domain.queue == RenderGraph::QueueRole::Copy ||
         pass.domain.pipeline == RenderGraph::PipelineType::Copy) &&
        !transfer_state) {
        return Fail("copy execution domain requires a transfer state in pass '" + pass.name + "'");
    }
    return true;
}

bool RenderGraphCompiler::BuildAtomicCells() {
    static constexpr std::array<TextureAspect, 3> texture_aspects{
        TextureAspect::Color, TextureAspect::Depth, TextureAspect::Stencil
    };

    for (uint32_t resource_index = 0; resource_index < graph.resources.size(); ++resource_index) {
        const auto& resource = graph.resources[resource_index];
        auto&       cells    = resource_cells[resource_index];

        if (resource.kind == ResourceKind::Texture) {
            std::vector<uint32_t> mip_boundaries{0, resource.texture_desc.mip_count};
            std::vector<uint32_t> layer_boundaries{0, resource.texture_desc.layer_count};
            auto add_texture_boundaries = [&](const ResourceRange& range) {
                mip_boundaries.push_back(range.texture.mip_first);
                mip_boundaries.push_back(range.texture.mip_first + range.texture.mip_count);
                layer_boundaries.push_back(range.texture.layer_first);
                layer_boundaries.push_back(range.texture.layer_first + range.texture.layer_count);
            };
            for (const auto& accesses : normalized_accesses) {
                for (const auto& access : accesses) {
                    if (access.resource.index != resource_index) {
                        continue;
                    }
                    add_texture_boundaries(access.range);
                }
            }
            for (const auto& state : normalized_initial_states[resource_index]) {
                add_texture_boundaries(state.range);
            }
            for (const auto& state : normalized_final_states[resource_index]) {
                add_texture_boundaries(state.range);
            }
            SortUnique(mip_boundaries);
            SortUnique(layer_boundaries);

            for (TextureAspect aspect : texture_aspects) {
                if (!HasAspect(resource.texture_desc.aspects, aspect)) {
                    continue;
                }
                for (uint32_t mip_index = 0; mip_index + 1 < mip_boundaries.size(); ++mip_index) {
                    for (uint32_t layer_index = 0; layer_index + 1 < layer_boundaries.size(); ++layer_index) {
                        const uint32_t mip_first   = mip_boundaries[mip_index];
                        const uint32_t mip_end     = mip_boundaries[mip_index + 1];
                        const uint32_t layer_first = layer_boundaries[layer_index];
                        const uint32_t layer_end   = layer_boundaries[layer_index + 1];
                        if (mip_first == mip_end || layer_first == layer_end) {
                            continue;
                        }
                        AtomicCell cell{};
                        cell.range       = ResourceRange::Texture(RenderGraph::TextureRange{
                                  .aspects     = aspect,
                                  .mip_first   = mip_first,
                                  .mip_count   = mip_end - mip_first,
                                  .layer_first = layer_first,
                                  .layer_count = layer_end - layer_first
                        });
                        cell.initialized = resource.imported;
                        cell.version     = resource.imported ? 0 : RenderGraph::InvalidVersion;
                        cell.state       = RenderGraph::ResourceState::Texture(
                            resource.imported ? RenderGraph::TextureState::Automatic :
                                                RenderGraph::TextureState::Undefined
                        );
                        cell.state_known = !resource.imported;
                        cells.push_back(std::move(cell));
                    }
                }
            }
        } else if (resource.kind == ResourceKind::Buffer && resource.buffer_desc.byte_size != 0) {
            std::vector<uint64_t> boundaries{0, resource.buffer_desc.byte_size};
            auto add_buffer_boundaries = [&](const ResourceRange& range) {
                boundaries.push_back(range.buffer.offset);
                boundaries.push_back(range.buffer.offset + range.buffer.size);
            };
            for (const auto& accesses : normalized_accesses) {
                for (const auto& access : accesses) {
                    if (access.resource.index != resource_index) {
                        continue;
                    }
                    add_buffer_boundaries(access.range);
                }
            }
            for (const auto& state : normalized_initial_states[resource_index]) {
                add_buffer_boundaries(state.range);
            }
            for (const auto& state : normalized_final_states[resource_index]) {
                add_buffer_boundaries(state.range);
            }
            SortUnique(boundaries);
            for (uint32_t index = 0; index + 1 < boundaries.size(); ++index) {
                if (boundaries[index] == boundaries[index + 1]) {
                    continue;
                }
                AtomicCell cell{};
                cell.range       = ResourceRange::Buffer(RenderGraph::BufferRange{
                          .offset = boundaries[index], .size = boundaries[index + 1] - boundaries[index]
                });
                cell.initialized = resource.imported;
                cell.version     = resource.imported ? 0 : RenderGraph::InvalidVersion;
                cell.state       = RenderGraph::ResourceState::Buffer(
                    resource.imported ? RenderGraph::BufferState::Automatic :
                                        RenderGraph::BufferState::Undefined
                );
                cell.state_known = !resource.imported;
                cells.push_back(std::move(cell));
            }
        } else {
            AtomicCell cell{};
            cell.range       = ResourceRange::Whole(resource.kind);
            cell.initialized = resource.imported;
            cell.version     = resource.imported ? 0 : RenderGraph::InvalidVersion;
            if (resource.kind == ResourceKind::Buffer) {
                cell.state = RenderGraph::ResourceState::Buffer(
                    resource.imported ? RenderGraph::BufferState::Automatic :
                                        RenderGraph::BufferState::Undefined
                );
            } else {
                cell.state = RenderGraph::ResourceState::Token();
            }
            cell.state_known = resource.kind == ResourceKind::Token || !resource.imported;
            cells.push_back(std::move(cell));
        }

        if (cells.empty()) {
            return Fail("resource produced no compiler intervals: " + resource.name);
        }

        for (const auto& initial : normalized_initial_states[resource_index]) {
            bool matched = false;
            for (auto& cell : cells) {
                if (!RangesOverlap(initial.range, cell.range)) {
                    continue;
                }
                matched = true;
                if (cell.has_explicit_initial_state &&
                    (cell.state != initial.state || cell.last_domain.queue != initial.queue ||
                     cell.last_mode != initial.boundary_access)) {
                    return Fail("overlapping initial states conflict on resource '" + resource.name + "'");
                }
                cell.state                       = initial.state;
                cell.state_known                 = true;
                cell.initialized                 = !initial.state.IsUndefined();
                if (!cell.initialized) {
                    cell.version = RenderGraph::InvalidVersion;
                }
                cell.last_domain                 = RenderGraph::ExecutionDomain{
                    initial.queue, RenderGraph::PipelineType::None
                };
                cell.last_mode                    = initial.boundary_access;
                cell.has_explicit_initial_state  = true;
            }
            if (!matched) {
                return Fail("initial state did not map to a compiler interval on resource '" +
                            resource.name + "'");
            }
        }
    }
    return true;
}

bool RenderGraphCompiler::BuildSemanticDependencies() {
    for (uint32_t pass_index = 0; pass_index < graph.passes.size(); ++pass_index) {
        const auto& pass = graph.passes[pass_index];
        for (const auto dependency : pass.explicit_dependencies) {
            if (!graph.IsValidPass(dependency)) {
                return Fail("pass '" + pass.name + "' has an invalid explicit dependency");
            }
            AddEdge(
                dependency.index,
                pass_index,
                RenderGraph::CompiledEdgeReason{.kind = RenderGraph::EdgeReasonKind::Explicit}
            );
        }

        std::vector<std::vector<CellUsage>> usages(graph.resources.size());
        for (uint32_t resource_index = 0; resource_index < graph.resources.size(); ++resource_index) {
            usages[resource_index].resize(resource_cells[resource_index].size());
        }

        for (const auto& access : normalized_accesses[pass_index]) {
            auto&       resource_usages = usages[access.resource.index];
            const auto& cells           = resource_cells[access.resource.index];
            bool        matched         = false;
            for (uint32_t cell_index = 0; cell_index < cells.size(); ++cell_index) {
                if (!RangesOverlap(access.range, cells[cell_index].range)) {
                    continue;
                }
                matched     = true;
                auto& usage = resource_usages[cell_index];
                if (!MergeCellState(
                        pass_index, graph.resources[access.resource.index], usage, access
                    )) {
                    return false;
                }
            }
            if (!matched) {
                return Fail("access range did not map to a compiler interval in pass '" + pass.name + "'");
            }
        }

        for (uint32_t resource_index = 0; resource_index < graph.resources.size(); ++resource_index) {
            auto&      resource_usages = usages[resource_index];
            const bool writes_resource =
                std::any_of(resource_usages.begin(), resource_usages.end(), [](const CellUsage& usage) {
                    return usage.write;
                });
            const uint32_t output_version =
                writes_resource ? ++resource_version_counts[resource_index] : RenderGraph::InvalidVersion;

            for (uint32_t cell_index = 0; cell_index < resource_usages.size(); ++cell_index) {
                const CellUsage usage = resource_usages[cell_index];
                if (!usage.read && !usage.write) {
                    continue;
                }

                auto&          cell            = resource_cells[resource_index][cell_index];
                const uint32_t input_version   = cell.version;
                const auto     resource_handle = RenderGraph::ResourceHandle{resource_index, graph.graph_id};
                const auto&    resource        = graph.resources[resource_index];
                const auto     access_mode     = ToAccessMode(usage.read, usage.write);
                const auto     next_state      = usage.explicit_state ?
                                                    usage.state :
                                                    RenderGraph::ResourceState{.kind = resource.kind};

                const bool physical_resource = resource.kind != ResourceKind::Token;
                const bool source_state_unknown = physical_resource && !cell.state_known;
                const bool state_transition =
                    physical_resource && usage.explicit_state &&
                    (source_state_unknown || !StatesCompatible(cell.state, next_state));

                std::vector<RenderGraph::CompiledBarrierSource> barrier_sources{};
                auto append_source = [&](const RenderGraph::CompiledBarrierSource& source) {
                    if (!source.pass.IsValid()) {
                        return;
                    }
                    if (std::find(barrier_sources.begin(), barrier_sources.end(), source) ==
                        barrier_sources.end()) {
                        barrier_sources.push_back(source);
                    }
                };
                if (usage.read && cell.last_writer != RenderGraph::PassHandle::InvalidIndex) {
                    append_source(cell.last_writer_source);
                }
                if (usage.write) {
                    if (cell.last_writer != RenderGraph::PassHandle::InvalidIndex) {
                        append_source(cell.last_writer_source);
                    }
                    for (const auto& reader : cell.readers) {
                        append_source(reader);
                    }
                }

                const bool boundary_memory_dependency =
                    cell.last_access == RenderGraph::PassHandle::InvalidIndex &&
                    cell.last_mode != RenderGraph::AccessMode::None &&
                    (HasWrite(cell.last_mode) || usage.write);
                const bool memory_dependency =
                    physical_resource && (!barrier_sources.empty() || boundary_memory_dependency);

                bool queue_ownership = false;
                if (physical_resource && cell.last_domain.queue != RenderGraph::QueueRole::None) {
                    const auto src_queue = graph.queue_topology.Resolve(cell.last_domain.queue);
                    const auto dst_queue = graph.queue_topology.Resolve(pass.domain.queue);
                    queue_ownership = IsExclusive(resource) &&
                                      src_queue.family_id != dst_queue.family_id;
                }

                if (state_transition || queue_ownership) {
                    for (const auto& reader : cell.readers) {
                        append_source(reader);
                    }
                    if (queue_ownership) {
                        // Only accesses in the currently owning family can participate in the
                        // next release. Older-family readers are already ordered through the
                        // ownership chain and must not receive another release placement.
                        RetainCurrentOwnerFamilySources(
                            barrier_sources, graph.queue_topology, cell.last_domain.queue
                        );
                    }
                    if (cell.last_access != RenderGraph::PassHandle::InvalidIndex &&
                        barrier_sources.empty()) {
                        append_source(RenderGraph::CompiledBarrierSource{
                            .pass   = RenderGraph::PassHandle{cell.last_access, graph.graph_id},
                            .state  = cell.state,
                            .access = cell.last_mode,
                            .domain = cell.last_domain,
                        });
                    }
                    for (const auto& source : barrier_sources) {
                        if (state_transition) {
                            AddEdge(
                                source.pass.index,
                                pass_index,
                                RenderGraph::CompiledEdgeReason{
                                    .kind           = RenderGraph::EdgeReasonKind::StateTransition,
                                    .resource       = resource_handle,
                                    .range          = cell.range,
                                    .input_version  = cell.version,
                                    .output_version = cell.version
                                }
                            );
                        }
                        if (queue_ownership) {
                            AddEdge(
                                source.pass.index,
                                pass_index,
                                RenderGraph::CompiledEdgeReason{
                                    .kind           = RenderGraph::EdgeReasonKind::QueueOwnership,
                                    .resource       = resource_handle,
                                    .range          = cell.range,
                                    .input_version  = cell.version,
                                    .output_version = cell.version
                                }
                            );
                        }
                    }
                }

                if (physical_resource && !state_transition && !queue_ownership &&
                    cell.availability_established_pass != RenderGraph::PassHandle::InvalidIndex &&
                    cell.availability_established_pass != pass_index) {
                    AddEdge(
                        cell.availability_established_pass,
                        pass_index,
                        RenderGraph::CompiledEdgeReason{
                            .kind           = RenderGraph::EdgeReasonKind::StateTransition,
                            .resource       = resource_handle,
                            .range          = cell.range,
                            .input_version  = cell.version,
                            .output_version = cell.version
                        }
                    );
                }
                if (!state_transition && !queue_ownership &&
                    cell.ownership_established_pass != RenderGraph::PassHandle::InvalidIndex &&
                    cell.ownership_established_pass != pass_index) {
                    const auto& established_pass = graph.passes[cell.ownership_established_pass];
                    if (graph.queue_topology.Resolve(established_pass.domain.queue).native_queue_id !=
                        graph.queue_topology.Resolve(pass.domain.queue).native_queue_id) {
                        AddEdge(
                            cell.ownership_established_pass,
                            pass_index,
                            RenderGraph::CompiledEdgeReason{
                                .kind           = RenderGraph::EdgeReasonKind::QueueOwnership,
                                .resource       = resource_handle,
                                .range          = cell.range,
                                .input_version  = cell.version,
                                .output_version = cell.version
                            }
                        );
                    }
                }

                const bool import_boundary = resource.imported &&
                                             cell.last_access == RenderGraph::PassHandle::InvalidIndex;
                if (physical_resource &&
                    (state_transition || memory_dependency || queue_ownership ||
                     (import_boundary && cell.has_explicit_initial_state))) {
                    AddBarrier(
                        resource_index,
                        cell,
                        RenderGraph::PassHandle{pass_index, graph.graph_id},
                        next_state,
                        pass.domain,
                        access_mode,
                        state_transition,
                        memory_dependency,
                        queue_ownership,
                        import_boundary,
                        false,
                        source_state_unknown,
                        std::move(barrier_sources)
                    );
                }
                if (source_state_unknown && usage.explicit_state) {
                    graph.compiled_plan.state_plan_complete = false;
                }

                if (usage.read) {
                    if (!cell.initialized) {
                        return Fail(
                            "transient resource '" + graph.resources[resource_index].name +
                            "' is read before its first producer in pass '" + pass.name + "'"
                        );
                    }
                    if (cell.last_writer != RenderGraph::PassHandle::InvalidIndex) {
                        AddEdge(
                            cell.last_writer,
                            pass_index,
                            RenderGraph::CompiledEdgeReason{
                                .kind           = RenderGraph::EdgeReasonKind::ReadAfterWrite,
                                .resource       = resource_handle,
                                .range          = cell.range,
                                .input_version  = cell.version,
                                .output_version = cell.version
                            }
                        );
                    }
                }

                if (usage.write) {
                    if (cell.last_writer != RenderGraph::PassHandle::InvalidIndex) {
                        AddEdge(
                            cell.last_writer,
                            pass_index,
                            RenderGraph::CompiledEdgeReason{
                                .kind           = RenderGraph::EdgeReasonKind::WriteAfterWrite,
                                .resource       = resource_handle,
                                .range          = cell.range,
                                .input_version  = cell.version,
                                .output_version = output_version
                            }
                        );
                    }
                    for (const auto& reader : cell.readers) {
                        AddEdge(
                            reader.pass.index,
                            pass_index,
                            RenderGraph::CompiledEdgeReason{
                                .kind           = RenderGraph::EdgeReasonKind::WriteAfterRead,
                                .resource       = resource_handle,
                                .range          = cell.range,
                                .input_version  = cell.version,
                                .output_version = output_version
                            }
                        );
                    }
                    cell.readers.clear();
                    cell.last_writer                      = pass_index;
                    cell.last_writer_source               = RenderGraph::CompiledBarrierSource{
                        .pass   = RenderGraph::PassHandle{pass_index, graph.graph_id},
                        .state  = next_state,
                        .access = access_mode,
                        .domain = pass.domain,
                    };
                    cell.version                          = output_version;
                    cell.initialized                      = true;
                    resource_ever_written[resource_index] = true;
                } else {
                    if (state_transition || queue_ownership) {
                        // The current read is ordered after the complete old frontier and becomes
                        // the only reader needed for later state/ownership transitions. Same-state
                        // concurrent reads keep accumulating above because neither flag is set.
                        cell.readers.clear();
                        // The previous writer's visibility and dependency are now carried
                        // transitively by this read. Preserve the logical version, but drop the
                        // old writer endpoint so queue planning cannot lower a stale direct sync.
                        cell.last_writer        = RenderGraph::PassHandle::InvalidIndex;
                        cell.last_writer_source = {};
                    }
                    const auto reader = RenderGraph::CompiledBarrierSource{
                        .pass   = RenderGraph::PassHandle{pass_index, graph.graph_id},
                        .state  = next_state,
                        .access = access_mode,
                        .domain = pass.domain,
                    };
                    if (std::find(cell.readers.begin(), cell.readers.end(), reader) ==
                        cell.readers.end()) {
                        cell.readers.push_back(reader);
                    }
                }

                graph.compiled_plan.accesses.push_back(RenderGraph::CompiledAccess{
                    .pass           = RenderGraph::PassHandle{pass_index, graph.graph_id},
                    .resource       = resource_handle,
                    .mode           = access_mode,
                    .range          = cell.range,
                    .state          = next_state,
                    .domain         = pass.domain,
                    .input_version  = input_version,
                    .output_version = usage.write ? output_version : input_version
                });

                cell.last_access = pass_index;
                cell.last_domain = pass.domain;
                cell.last_mode   = access_mode;
                cell.state       = next_state;
                cell.state_known = usage.explicit_state;
                const bool import_availability = import_boundary && memory_dependency;
                if (physical_resource &&
                    (usage.write || import_availability || state_transition || queue_ownership)) {
                    // This pass has ordered the complete prior frontier and becomes the source of
                    // state/data availability for later accesses. Automatic accesses still inherit
                    // this dependency even though they make the logical state unknown.
                    cell.availability_established_pass = pass_index;
                }
                if (((usage.write || import_availability) && IsExclusive(resource)) ||
                    queue_ownership ||
                    (state_transition && IsExclusive(resource))) {
                    // Writes and import-memory barriers similarly become the authoritative
                    // frontier inside the current owner family, so sibling native queues never
                    // bypass external availability or wait on an older acquire.
                    cell.ownership_established_pass = pass_index;
                }
            }
        }
    }

    for (uint32_t resource_index = 0; resource_index < graph.resources.size(); ++resource_index) {
        const auto& resource = graph.resources[resource_index];
        const bool has_gpu_access = std::any_of(
            graph.compiled_plan.accesses.begin(),
            graph.compiled_plan.accesses.end(),
            [&](const RenderGraph::CompiledAccess& access) {
                return access.resource.index == resource_index;
            }
        );
        const bool has_opaque_reference = std::any_of(
            graph.passes.begin(),
            graph.passes.end(),
            [&](const RenderGraph::PassDeclaration& pass) {
                return std::any_of(
                    pass.references.begin(),
                    pass.references.end(),
                    [&](RenderGraph::ResourceHandle reference) {
                        return reference.index == resource_index;
                    }
                );
            }
        );
        if (!resource.imported && (has_gpu_access || has_opaque_reference) &&
            !resource_ever_written[resource_index]) {
            return Fail("transient resource has no producer: " + resource.name);
        }
        if (resource.exported) {
            const auto& final_states = normalized_final_states[resource_index];
            const bool exported_ranges_initialized = std::all_of(
                resource_cells[resource_index].begin(),
                resource_cells[resource_index].end(),
                [&](const AtomicCell& cell) {
                    bool read_boundary = false;
                    bool typed_exported_cell = false;
                    for (const auto& final : final_states) {
                        if (RangesOverlap(final.range, cell.range)) {
                            typed_exported_cell = true;
                            read_boundary |= HasRead(final.boundary_access);
                        }
                    }
                    const bool exported_cell = resource.whole_resource_exported || typed_exported_cell;
                    const bool contents_required = !resource.imported || read_boundary;
                    return !exported_cell || !contents_required || cell.initialized;
                }
            );
            if (!exported_ranges_initialized) {
                return Fail(
                    std::string(
                        resource.imported ?
                            "exported imported resource has uninitialized subresources required by a read boundary: " :
                            "exported transient resource has uninitialized subresources: "
                    ) + resource.name
                );
            }
        }
    }
    return true;
}

bool RenderGraphCompiler::MergeCellState(
    uint32_t                                pass_index,
    const RenderGraph::ResourceDeclaration& resource,
    CellUsage&                              usage,
    const NormalizedAccess&                 access
) {
    const bool had_access = usage.read || usage.write;
    usage.read |= HasRead(access.mode);
    usage.write |= HasWrite(access.mode);

    if (!had_access) {
        usage.state          = access.state;
        usage.explicit_state = access.explicit_state;
    } else if (access.explicit_state) {
        if (usage.explicit_state) {
            if (!StatesCompatible(usage.state, access.state)) {
                return Fail("pass '" + graph.passes[pass_index].name +
                            "' requests conflicting states for one atomic cell of resource '" +
                            resource.name + "'");
            }
            usage.state = CanonicalCompatibleState(usage.state, access.state);
        } else {
            usage.state          = access.state;
            usage.explicit_state = true;
        }
    }

    if (usage.explicit_state) {
        const auto combined_mode = ToAccessMode(usage.read, usage.write);
        const bool supported = resource.kind == ResourceKind::Texture ?
                                   TextureStateSupports(usage.state.texture, combined_mode) :
                               resource.kind == ResourceKind::Buffer ?
                                   BufferStateSupports(usage.state.buffer, combined_mode) :
                                   true;
        if (!supported) {
            return Fail("combined accesses in pass '" + graph.passes[pass_index].name +
                        "' are incompatible with the explicit state of resource '" + resource.name +
                        "'");
        }
    }
    return true;
}

void RenderGraphCompiler::AddBarrier(
    uint32_t                     resource_index,
    const AtomicCell&            cell,
    RenderGraph::PassHandle      dst_pass,
    RenderGraph::ResourceState   next_state,
    RenderGraph::ExecutionDomain next_domain,
    RenderGraph::AccessMode      next_mode,
    bool                         state_transition,
    bool                         memory_dependency,
    bool                         queue_ownership,
    bool                         import_boundary,
    bool                         export_boundary,
    bool                         source_state_unknown,
    std::vector<RenderGraph::CompiledBarrierSource> sources
) {
    const auto primary_source = std::max_element(
        sources.begin(),
        sources.end(),
        [](const RenderGraph::CompiledBarrierSource& lhs,
           const RenderGraph::CompiledBarrierSource& rhs) {
            return lhs.pass.index < rhs.pass.index;
        }
    );
    const auto src_pass = primary_source == sources.end() ? RenderGraph::PassHandle{} :
                                                            primary_source->pass;
    const auto src_domain = primary_source == sources.end() ? cell.last_domain :
                                                              primary_source->domain;
    const auto src_access = primary_source == sources.end() ? cell.last_mode :
                                                              primary_source->access;
    const bool execution_dependency = dst_pass.IsValid() && !sources.empty();
    bool       queue_dependency     = false;
    if (next_domain.queue != RenderGraph::QueueRole::None) {
        const auto dst_native = graph.queue_topology.Resolve(next_domain.queue).native_queue_id;
        if (sources.empty()) {
            queue_dependency = cell.last_domain.queue != RenderGraph::QueueRole::None &&
                               graph.queue_topology.Resolve(cell.last_domain.queue).native_queue_id !=
                                   dst_native;
        } else {
            for (const auto& source : sources) {
                if (source.domain.queue != RenderGraph::QueueRole::None &&
                    graph.queue_topology.Resolve(source.domain.queue).native_queue_id != dst_native) {
                    queue_dependency = true;
                    break;
                }
            }
        }
    }
    graph.compiled_plan.barriers.push_back(RenderGraph::CompiledBarrier{
        .resource             = RenderGraph::ResourceHandle{resource_index, graph.graph_id},
        .range                = cell.range,
        .src_pass             = src_pass,
        .dst_pass             = dst_pass,
        .before_state         = cell.state,
        .after_state          = next_state,
        .before_access        = src_access,
        .after_access         = next_mode,
        .src_domain           = src_domain,
        .dst_domain           = next_domain,
        .state_transition     = state_transition,
        .memory_dependency    = memory_dependency,
        .execution_dependency = execution_dependency,
        .queue_dependency     = queue_dependency,
        .queue_ownership      = queue_ownership,
        .discard_previous_contents = cell.state_known && cell.state.IsUndefined(),
        .import_boundary      = import_boundary,
        .export_boundary      = export_boundary,
        .source_state_unknown = source_state_unknown,
        .sources              = std::move(sources)
    });
}

bool RenderGraphCompiler::BuildFinalBarriers() {
    for (uint32_t resource_index = 0; resource_index < graph.resources.size(); ++resource_index) {
        const auto& resource = graph.resources[resource_index];
        const auto& finals   = normalized_final_states[resource_index];
        if (finals.empty()) {
            continue;
        }
        for (const auto& cell : resource_cells[resource_index]) {
            const NormalizedStateDeclaration* final = nullptr;
            for (const auto& candidate : finals) {
                if (!RangesOverlap(candidate.range, cell.range)) {
                    continue;
                }
                if (final != nullptr &&
                    (final->state != candidate.state || final->queue != candidate.queue ||
                     final->boundary_access != candidate.boundary_access)) {
                    return Fail("overlapping final states conflict on resource '" + resource.name + "'");
                }
                final = &candidate;
            }
            if (final == nullptr) {
                continue;
            }

            const bool source_state_unknown = !cell.state_known;
            const bool state_transition =
                source_state_unknown || !StatesCompatible(cell.state, final->state);
            std::vector<RenderGraph::CompiledBarrierSource> sources = cell.readers;
            if (cell.last_writer_source.pass.IsValid() &&
                std::find(sources.begin(), sources.end(), cell.last_writer_source) == sources.end()) {
                sources.push_back(cell.last_writer_source);
            }
            bool queue_ownership = false;
            if (cell.last_domain.queue != RenderGraph::QueueRole::None) {
                const auto src_queue = graph.queue_topology.Resolve(cell.last_domain.queue);
                const auto dst_queue = graph.queue_topology.Resolve(final->queue);
                queue_ownership = IsExclusive(resource) && src_queue.family_id != dst_queue.family_id;
            }
            if (queue_ownership) {
                RetainCurrentOwnerFamilySources(
                    sources, graph.queue_topology, cell.last_domain.queue
                );
                if (sources.empty() && cell.last_access != RenderGraph::PassHandle::InvalidIndex) {
                    sources.push_back(RenderGraph::CompiledBarrierSource{
                        .pass   = RenderGraph::PassHandle{cell.last_access, graph.graph_id},
                        .state  = cell.state,
                        .access = cell.last_mode,
                        .domain = cell.last_domain,
                    });
                }
            }
            const bool source_write = std::any_of(
                sources.begin(), sources.end(), [](const RenderGraph::CompiledBarrierSource& source) {
                    return HasWrite(source.access);
                }
            );
            const bool boundary_source_write = sources.empty() && HasWrite(cell.last_mode);
            const bool memory_dependency =
                final->boundary_access != RenderGraph::AccessMode::None &&
                (source_write || boundary_source_write || HasWrite(final->boundary_access));
            AddBarrier(
                resource_index,
                cell,
                RenderGraph::PassHandle{},
                final->state,
                RenderGraph::ExecutionDomain{final->queue, RenderGraph::PipelineType::None},
                final->boundary_access,
                state_transition,
                memory_dependency,
                queue_ownership,
                false,
                true,
                source_state_unknown,
                std::move(sources)
            );
            if (source_state_unknown) {
                graph.compiled_plan.state_plan_complete = false;
            }
        }
    }
    return true;
}

void RenderGraphCompiler::AuditStatePlanCompleteness() {
    for (uint32_t resource_index = 0; resource_index < graph.resources.size(); ++resource_index) {
        const auto& resource = graph.resources[resource_index];
        if (resource.kind == ResourceKind::Token) {
            continue;
        }
        const auto& final_states = normalized_final_states[resource_index];
        for (const auto& cell : resource_cells[resource_index]) {
            const bool exported_cell = resource.whole_resource_exported ||
                                       std::any_of(
                                           final_states.begin(),
                                           final_states.end(),
                                           [&](const NormalizedStateDeclaration& final) {
                                               return RangesOverlap(final.range, cell.range);
                                           }
                                       );
            const bool participates_in_plan = exported_cell ||
                                              cell.last_access != RenderGraph::PassHandle::InvalidIndex;
            if (participates_in_plan && !cell.state_known) {
                graph.compiled_plan.state_plan_complete = false;
                return;
            }
        }
    }
}

void RenderGraphCompiler::AddEdge(uint32_t src, uint32_t dst, RenderGraph::CompiledEdgeReason reason) {
    if (src == dst || src == RenderGraph::PassHandle::InvalidIndex ||
        dst == RenderGraph::PassHandle::InvalidIndex) {
        return;
    }

    const auto src_handle = RenderGraph::PassHandle{src, graph.graph_id};
    const auto dst_handle = RenderGraph::PassHandle{dst, graph.graph_id};
    auto       edge       = std::find_if(
        graph.compiled_plan.edges.begin(),
        graph.compiled_plan.edges.end(),
        [&](const RenderGraph::CompiledEdge& candidate) {
            return candidate.src == src_handle && candidate.dst == dst_handle;
        }
    );
    if (edge == graph.compiled_plan.edges.end()) {
        graph.compiled_plan.edges.push_back(
            RenderGraph::CompiledEdge{src_handle, dst_handle, {std::move(reason)}}
        );
        return;
    }
    if (std::find(edge->reasons.begin(), edge->reasons.end(), reason) == edge->reasons.end()) {
        edge->reasons.push_back(std::move(reason));
    }
}

bool RenderGraphCompiler::BuildTopologicalOrder() {
    std::sort(
        graph.compiled_plan.edges.begin(),
        graph.compiled_plan.edges.end(),
        [](const RenderGraph::CompiledEdge& lhs, const RenderGraph::CompiledEdge& rhs) {
            return lhs.src.index < rhs.src.index ||
                   (lhs.src.index == rhs.src.index && lhs.dst.index < rhs.dst.index);
        }
    );
    for (auto& edge : graph.compiled_plan.edges) {
        std::sort(edge.reasons.begin(), edge.reasons.end(), ReasonLess);
    }

    std::vector<uint32_t> indegree(graph.passes.size(), 0);
    for (const auto& edge : graph.compiled_plan.edges) {
        ++indegree[edge.dst.index];
    }

    std::vector<bool> scheduled(graph.passes.size(), false);
    while (graph.compiled_plan.topological_order.size() < graph.passes.size()) {
        uint32_t next = RenderGraph::PassHandle::InvalidIndex;
        for (uint32_t pass_index = 0; pass_index < graph.passes.size(); ++pass_index) {
            if (!scheduled[pass_index] && indegree[pass_index] == 0) {
                next = pass_index;
                break;
            }
        }
        if (next == RenderGraph::PassHandle::InvalidIndex) {
            return Fail("pass dependency cycle detected");
        }
        scheduled[next] = true;
        graph.compiled_plan.topological_order.push_back(RenderGraph::PassHandle{next, graph.graph_id});
        for (const auto& edge : graph.compiled_plan.edges) {
            if (edge.src.index == next) {
                assert(indegree[edge.dst.index] > 0);
                --indegree[edge.dst.index];
            }
        }
    }
    return true;
}

bool RenderGraphCompiler::BuildExecutionOrder() {
    graph.compiled_plan.execution_order.reserve(graph.passes.size());
    for (uint32_t pass_index = 0; pass_index < graph.passes.size(); ++pass_index) {
        graph.compiled_plan.execution_order.push_back(RenderGraph::PassHandle{pass_index, graph.graph_id});
    }

    for (const auto& edge : graph.compiled_plan.edges) {
        if (edge.src.index >= edge.dst.index) {
            return Fail("serial declaration order cannot satisfy a compiled dependency");
        }
    }
    return true;
}

void RenderGraphCompiler::BuildQueuePlan() {
    std::vector<uint32_t> pass_to_batch(
        graph.passes.size(), RenderGraph::PassHandle::InvalidIndex
    );
    bool force_new_managed_batch = false;
    for (const auto pass_handle : graph.compiled_plan.execution_order) {
        const auto& pass    = graph.passes[pass_handle.index];
        if (pass.execution_class == RenderGraph::PassExecutionClass::CpuPrepare) {
            continue;
        }
        const auto  binding = graph.queue_topology.Resolve(pass.domain.queue);
        if (pass.execution_class == RenderGraph::PassExecutionClass::ExternalControl) {
            const uint32_t batch_id =
                static_cast<uint32_t>(graph.compiled_plan.queue_batches.size());
            graph.compiled_plan.queue_batches.push_back(RenderGraph::CompiledQueueBatch{
                .id               = batch_id,
                .queue            = binding,
                .passes           = {pass_handle},
                .external_control = true,
            });
            pass_to_batch[pass_handle.index] = batch_id;
            force_new_managed_batch          = true;
            continue;
        }
        if (graph.compiled_plan.queue_batches.empty() || force_new_managed_batch ||
            graph.compiled_plan.queue_batches.back().external_control ||
            graph.compiled_plan.queue_batches.back().queue.role != binding.role) {
            const uint32_t batch_id =
                static_cast<uint32_t>(graph.compiled_plan.queue_batches.size());
            graph.compiled_plan.queue_batches.push_back(RenderGraph::CompiledQueueBatch{
                .id = batch_id,
                .queue = binding,
            });
        }
        force_new_managed_batch = false;
        auto& batch = graph.compiled_plan.queue_batches.back();
        batch.passes.push_back(pass_handle);
        pass_to_batch[pass_handle.index] = batch.id;
    }

    graph.compiled_plan.pass_barriers.reserve(graph.passes.size());
    for (uint32_t pass_index = 0; pass_index < graph.passes.size(); ++pass_index) {
        graph.compiled_plan.pass_barriers.push_back(RenderGraph::CompiledPassBarriers{
            .pass = RenderGraph::PassHandle{pass_index, graph.graph_id},
        });
    }

    auto add_batch_dependency = [&](
                                    uint32_t signal_batch,
                                    uint32_t wait_batch,
                                    bool     gpu_wait_required,
                                    uint32_t edge_index
                                ) -> RenderGraph::CompiledQueueSync& {
        auto existing = std::find_if(
            graph.compiled_plan.queue_syncs.begin(),
            graph.compiled_plan.queue_syncs.end(),
            [&](const RenderGraph::CompiledQueueSync& sync) {
                return sync.signal_batch == signal_batch && sync.wait_batch == wait_batch;
            }
        );
        if (existing != graph.compiled_plan.queue_syncs.end()) {
            existing->gpu_wait_required |= gpu_wait_required;
            if (edge_index != RenderGraph::PassHandle::InvalidIndex &&
                std::find(
                    existing->dependency_edges.begin(),
                    existing->dependency_edges.end(),
                    edge_index
                ) == existing->dependency_edges.end()) {
                existing->dependency_edges.push_back(edge_index);
            }
            return *existing;
        }

        auto& producer = graph.compiled_plan.queue_batches[signal_batch];
        auto& consumer = graph.compiled_plan.queue_batches[wait_batch];
        const uint32_t sync_id = static_cast<uint32_t>(graph.compiled_plan.queue_syncs.size());
        graph.compiled_plan.queue_syncs.push_back(RenderGraph::CompiledQueueSync{
            .id                = sync_id,
            .signal_pass       = producer.passes.back(),
            .wait_pass         = consumer.passes.front(),
            .signal_queue      = producer.queue,
            .wait_queue        = consumer.queue,
            .signal_batch      = signal_batch,
            .wait_batch        = wait_batch,
            .gpu_wait_required = gpu_wait_required,
            .dependency_edges  = edge_index == RenderGraph::PassHandle::InvalidIndex ?
                                     std::vector<uint32_t>{} :
                                     std::vector<uint32_t>{edge_index},
        });
        producer.signal_syncs.push_back(sync_id);
        consumer.wait_syncs.push_back(sync_id);
        return graph.compiled_plan.queue_syncs.back();
    };

    for (uint32_t edge_index = 0; edge_index < graph.compiled_plan.edges.size(); ++edge_index) {
        const auto& edge = graph.compiled_plan.edges[edge_index];
        const uint32_t signal_batch = pass_to_batch[edge.src.index];
        const uint32_t wait_batch   = pass_to_batch[edge.dst.index];
        if (signal_batch == RenderGraph::PassHandle::InvalidIndex ||
            wait_batch == RenderGraph::PassHandle::InvalidIndex) {
            continue;
        }
        if (signal_batch == wait_batch) {
            continue;
        }
        const auto& producer = graph.compiled_plan.queue_batches[signal_batch];
        const auto& consumer = graph.compiled_plan.queue_batches[wait_batch];
        add_batch_dependency(
            signal_batch,
            wait_batch,
            producer.queue.native_queue_id != consumer.queue.native_queue_id,
            edge_index
        );
    }

    for (uint32_t wait_batch = 0; wait_batch < graph.compiled_plan.queue_batches.size(); ++wait_batch) {
        const auto native_queue_id =
            graph.compiled_plan.queue_batches[wait_batch].queue.native_queue_id;
        for (uint32_t candidate = wait_batch; candidate > 0; --candidate) {
            const uint32_t signal_batch = candidate - 1;
            if (graph.compiled_plan.queue_batches[signal_batch].queue.native_queue_id ==
                native_queue_id) {
                add_batch_dependency(
                    signal_batch,
                    wait_batch,
                    false,
                    RenderGraph::PassHandle::InvalidIndex
                );
                break;
            }
        }
    }

    for (uint32_t barrier_index = 0; barrier_index < graph.compiled_plan.barriers.size();
         ++barrier_index) {
        const auto& barrier = graph.compiled_plan.barriers[barrier_index];
        if (barrier.import_boundary && !barrier.src_pass.IsValid()) {
            graph.compiled_plan.prologue_barriers.push_back(barrier_index);
        }
        if (barrier.export_boundary && !barrier.dst_pass.IsValid()) {
            graph.compiled_plan.epilogue_barriers.push_back(barrier_index);
        }
        if (barrier.dst_pass.IsValid()) {
            const uint32_t dst_batch = pass_to_batch[barrier.dst_pass.index];
            graph.compiled_plan.pass_barriers[barrier.dst_pass.index].before.push_back(barrier_index);
            if (!barrier.src_pass.IsValid() || barrier.queue_dependency) {
                graph.compiled_plan.queue_batches[dst_batch].pre_barriers.push_back(barrier_index);
            }
        }
        auto add_post_barrier = [&](RenderGraph::PassHandle source_pass) {
            if (!source_pass.IsValid() || !barrier.dst_pass.IsValid()) {
                return;
            }
            const uint32_t src_batch = pass_to_batch[source_pass.index];
            const uint32_t dst_batch = pass_to_batch[barrier.dst_pass.index];
            if (graph.compiled_plan.queue_batches[src_batch].queue.native_queue_id ==
                graph.compiled_plan.queue_batches[dst_batch].queue.native_queue_id) {
                return;
            }
            auto& post = graph.compiled_plan.queue_batches[src_batch].post_barriers;
            if (std::find(post.begin(), post.end(), barrier_index) == post.end()) {
                post.push_back(barrier_index);
            }
        };
        for (const auto& source : barrier.sources) {
            add_post_barrier(source.pass);
        }
        if (!barrier.dst_pass.IsValid() || !barrier.queue_dependency) {
            continue;
        }
        const uint32_t dst_batch = pass_to_batch[barrier.dst_pass.index];
        auto attach_to_sync = [&](RenderGraph::PassHandle source_pass) {
            if (!source_pass.IsValid()) {
                return;
            }
            const uint32_t src_batch = pass_to_batch[source_pass.index];
            if (graph.compiled_plan.queue_batches[src_batch].queue.native_queue_id ==
                graph.compiled_plan.queue_batches[dst_batch].queue.native_queue_id) {
                return;
            }
            auto sync = std::find_if(
                graph.compiled_plan.queue_syncs.begin(),
                graph.compiled_plan.queue_syncs.end(),
                [&](const RenderGraph::CompiledQueueSync& candidate) {
                    return candidate.signal_batch == src_batch && candidate.wait_batch == dst_batch;
                }
            );
            if (sync != graph.compiled_plan.queue_syncs.end() &&
                std::find(sync->barriers.begin(), sync->barriers.end(), barrier_index) ==
                    sync->barriers.end()) {
                sync->barriers.push_back(barrier_index);
            }
        };
        for (const auto& source : barrier.sources) {
            attach_to_sync(source.pass);
        }
    }
}

void RenderGraphCompiler::BuildDependencyWaves() {
    std::vector<uint32_t> indegree(graph.passes.size(), 0);
    for (const auto& edge : graph.compiled_plan.edges) {
        ++indegree[edge.dst.index];
    }

    std::vector<bool> scheduled(graph.passes.size(), false);
    uint32_t          scheduled_count = 0;
    while (scheduled_count < graph.passes.size()) {
        RenderGraph::CompiledWave wave{};
        for (uint32_t pass_index = 0; pass_index < graph.passes.size(); ++pass_index) {
            if (!scheduled[pass_index] && indegree[pass_index] == 0) {
                wave.passes.push_back(RenderGraph::PassHandle{pass_index, graph.graph_id});
            }
        }
        assert(!wave.passes.empty());
        for (const auto pass : wave.passes) {
            scheduled[pass.index] = true;
            ++scheduled_count;
        }
        for (const auto pass : wave.passes) {
            for (const auto& edge : graph.compiled_plan.edges) {
                if (edge.src == pass && !scheduled[edge.dst.index]) {
                    assert(indegree[edge.dst.index] > 0);
                    --indegree[edge.dst.index];
                }
            }
        }
        graph.compiled_plan.dependency_waves.push_back(std::move(wave));
    }
}

void RenderGraphCompiler::BuildRecordingBatches() {
    std::vector<uint32_t> pass_to_wave(
        graph.passes.size(),
        RenderGraph::PassHandle::InvalidIndex
    );
    for (uint32_t wave_index = 0; wave_index < graph.compiled_plan.dependency_waves.size();
         ++wave_index) {
        for (const auto pass : graph.compiled_plan.dependency_waves[wave_index].passes) {
            pass_to_wave[pass.index] = wave_index;
        }
    }

    graph.compiled_plan.recording_batches.reserve(graph.compiled_plan.execution_order.size());
    for (const auto pass_handle : graph.compiled_plan.execution_order) {
        const auto& pass = graph.passes[pass_handle.index];
        const auto  id = static_cast<uint32_t>(graph.compiled_plan.recording_batches.size());
        graph.compiled_plan.recording_batches.push_back(RenderGraph::CompiledRecordingBatch{
            .id              = id,
            .queue           = graph.queue_topology.Resolve(pass.domain.queue),
            .passes          = {pass_handle},
            .execution       = pass.execution_class,
            .workload        = pass.workload,
            .dependency_wave = pass_to_wave[pass_handle.index],
        });
    }
}

void RenderGraphCompiler::BuildLifetimes() {
    std::vector<uint32_t> execution_position(graph.passes.size(), RenderGraph::PassHandle::InvalidIndex);
    for (uint32_t position = 0; position < graph.compiled_plan.execution_order.size(); ++position) {
        execution_position[graph.compiled_plan.execution_order[position].index] = position;
    }

    for (const auto& access : graph.compiled_plan.accesses) {
        auto&          resource = graph.resources[access.resource.index];
        const uint32_t position = execution_position[access.pass.index];
        resource.first_use      = std::min(resource.first_use, position);
        resource.last_use       = resource.last_use == RenderGraph::PassHandle::InvalidIndex ?
                                      position :
                                      std::max(resource.last_use, position);
    }

    for (uint32_t pass_index = 0; pass_index < graph.passes.size(); ++pass_index) {
        const uint32_t position = execution_position[pass_index];
        for (const auto reference : graph.passes[pass_index].references) {
            auto& resource = graph.resources[reference.index];
            resource.first_use = std::min(resource.first_use, position);
            resource.last_use  = resource.last_use == RenderGraph::PassHandle::InvalidIndex ?
                                     position :
                                     std::max(resource.last_use, position);
        }
    }

    graph.compiled_plan.resources.reserve(graph.resources.size());
    for (uint32_t resource_index = 0; resource_index < graph.resources.size(); ++resource_index) {
        const auto& resource = graph.resources[resource_index];
        graph.compiled_plan.resources.push_back(RenderGraph::CompiledResource{
            .resource      = RenderGraph::ResourceHandle{resource_index, graph.graph_id},
            .first_use     = resource.first_use,
            .last_use      = resource.last_use,
            .version_count = resource_version_counts[resource_index],
            .transient_slot = RenderGraph::PassHandle::InvalidIndex,
            .imported      = resource.imported,
            .exported      = resource.exported
        });
    }
}

void RenderGraphCompiler::BuildTransientAliasPlan() {
    using CompiledAccess = RenderGraph::CompiledAccess;

    struct Candidate {
        uint32_t              resource_index = RenderGraph::PassHandle::InvalidIndex;
        const CompiledAccess* first_access   = nullptr;
        const CompiledAccess* last_access    = nullptr;
        bool                  reusable       = false;
    };

    struct AliasSlot {
        uint32_t                  id              = RenderGraph::PassHandle::InvalidIndex;
        uint32_t                  tail_resource   = RenderGraph::PassHandle::InvalidIndex;
        uint32_t                  available_after = RenderGraph::PassHandle::InvalidIndex;
    };

    const auto is_whole_range =
        [](const RenderGraph::ResourceDeclaration& resource,
           const RenderGraph::ResourceRange&       range) {
            if (range.kind != resource.kind) {
                return false;
            }
            if (resource.kind == ResourceKind::Texture) {
                return range.texture.aspects == resource.texture_desc.aspects &&
                       range.texture.mip_first == 0 &&
                       range.texture.mip_count == resource.texture_desc.mip_count &&
                       range.texture.layer_first == 0 &&
                       range.texture.layer_count == resource.texture_desc.layer_count;
            }
            if (resource.kind == ResourceKind::Buffer) {
                return range.buffer.offset == 0 &&
                       range.buffer.size == resource.buffer_desc.byte_size;
            }
            return false;
        };

    const auto descriptors_match =
        [&](uint32_t lhs_index, uint32_t rhs_index) {
            const auto& lhs = graph.resources[lhs_index];
            const auto& rhs = graph.resources[rhs_index];
            if (lhs.kind != rhs.kind) {
                return false;
            }
            if (lhs.kind == ResourceKind::Texture) {
                return lhs.transient_texture_desc.has_value() &&
                       rhs.transient_texture_desc.has_value() &&
                       *lhs.transient_texture_desc == *rhs.transient_texture_desc;
            }
            if (lhs.kind == ResourceKind::Buffer) {
                return lhs.transient_buffer_desc.has_value() &&
                       rhs.transient_buffer_desc.has_value() &&
                       *lhs.transient_buffer_desc == *rhs.transient_buffer_desc;
            }
            return false;
        };

    std::vector<uint32_t> execution_position(
        graph.passes.size(),
        RenderGraph::PassHandle::InvalidIndex
    );
    for (uint32_t position = 0; position < graph.compiled_plan.execution_order.size();
         ++position) {
        execution_position[graph.compiled_plan.execution_order[position].index] = position;
    }

    std::vector<Candidate> candidates{};
    candidates.reserve(graph.resources.size());
    for (uint32_t resource_index = 0; resource_index < graph.resources.size();
         ++resource_index) {
        auto&       compiled_resource = graph.compiled_plan.resources[resource_index];
        const auto& resource          = graph.resources[resource_index];
        const bool  allocation_backed =
            !resource.imported &&
            (resource.transient_texture_desc.has_value() ||
             resource.transient_buffer_desc.has_value());
        if (!allocation_backed ||
            compiled_resource.first_use == RenderGraph::PassHandle::InvalidIndex) {
            continue;
        }

        Candidate candidate{.resource_index = resource_index};
        std::vector<const CompiledAccess*> accesses{};
        for (const auto& access : graph.compiled_plan.accesses) {
            if (access.resource.index == resource_index) {
                accesses.push_back(&access);
            }
        }

        const bool has_opaque_reference =
            std::any_of(
                graph.passes.begin(),
                graph.passes.end(),
                [&](const RenderGraph::PassDeclaration& pass) {
                    return std::any_of(
                        pass.references.begin(),
                        pass.references.end(),
                        [&](RenderGraph::ResourceHandle reference) {
                            return reference.index == resource_index;
                        }
                    );
                }
            );

        uint32_t first_access_count = 0;
        uint32_t last_access_count  = 0;
        bool     accesses_are_safe  = !accesses.empty();
        for (const auto* access : accesses) {
            const uint32_t position = execution_position[access->pass.index];
            if (position == compiled_resource.first_use) {
                candidate.first_access = access;
                ++first_access_count;
            }
            if (position == compiled_resource.last_use) {
                candidate.last_access = access;
                ++last_access_count;
            }
            accesses_are_safe &=
                position != RenderGraph::PassHandle::InvalidIndex &&
                access->domain.queue == RenderGraph::QueueRole::Graphics &&
                !access->state.IsAutomatic() && !access->state.IsUndefined() &&
                is_whole_range(resource, access->range);
        }

        candidate.reusable =
            !resource.exported && !has_opaque_reference && accesses_are_safe &&
            resource_cells[resource_index].size() == 1 &&
            first_access_count == 1 && last_access_count == 1 &&
            candidate.first_access != nullptr && candidate.last_access != nullptr &&
            candidate.first_access->mode == RenderGraph::AccessMode::Write;
        candidates.push_back(candidate);
    }

    std::sort(
        candidates.begin(),
        candidates.end(),
        [&](const Candidate& lhs, const Candidate& rhs) {
            const auto& lhs_resource =
                graph.compiled_plan.resources[lhs.resource_index];
            const auto& rhs_resource =
                graph.compiled_plan.resources[rhs.resource_index];
            return lhs_resource.first_use < rhs_resource.first_use ||
                   (lhs_resource.first_use == rhs_resource.first_use &&
                    lhs.resource_index < rhs.resource_index);
        }
    );

    uint32_t               next_slot = 0;
    std::vector<AliasSlot> reusable_slots{};
    for (const auto& candidate : candidates) {
        auto& compiled_resource =
            graph.compiled_plan.resources[candidate.resource_index];

        AliasSlot* selected_slot = nullptr;
        if (candidate.reusable) {
            for (auto& slot : reusable_slots) {
                if (slot.available_after >= compiled_resource.first_use ||
                    !descriptors_match(slot.tail_resource, candidate.resource_index)) {
                    continue;
                }
                if (selected_slot == nullptr ||
                    slot.available_after > selected_slot->available_after ||
                    (slot.available_after == selected_slot->available_after &&
                     slot.id < selected_slot->id)) {
                    selected_slot = &slot;
                }
            }
        }

        std::vector<RenderGraph::CompiledBarrierSource> alias_sources{};
        const AtomicCell* predecessor_cell = nullptr;
        const RenderGraph::CompiledBarrierSource* primary_source = nullptr;
        if (selected_slot != nullptr &&
            resource_cells[selected_slot->tail_resource].size() == 1) {
            predecessor_cell =
                &resource_cells[selected_slot->tail_resource].front();
            alias_sources = predecessor_cell->readers;
            if (predecessor_cell->last_writer_source.pass.IsValid() &&
                std::find(
                    alias_sources.begin(),
                    alias_sources.end(),
                    predecessor_cell->last_writer_source
                ) == alias_sources.end()) {
                alias_sources.push_back(
                    predecessor_cell->last_writer_source
                );
            }
            std::sort(
                alias_sources.begin(),
                alias_sources.end(),
                [&](const RenderGraph::CompiledBarrierSource& lhs,
                    const RenderGraph::CompiledBarrierSource& rhs) {
                    return execution_position[lhs.pass.index] <
                           execution_position[rhs.pass.index];
                }
            );
            alias_sources.erase(
                std::unique(alias_sources.begin(), alias_sources.end()),
                alias_sources.end()
            );
            if (!alias_sources.empty()) {
                primary_source = &alias_sources.back();
            }
        }

        RenderGraph::CompiledBarrier* alias_barrier       = nullptr;
        uint32_t alias_barrier_index =
            RenderGraph::PassHandle::InvalidIndex;
        if (selected_slot != nullptr && predecessor_cell != nullptr &&
            primary_source != nullptr &&
            execution_position[primary_source->pass.index] <
                compiled_resource.first_use) {
            for (uint32_t barrier_index = 0;
                 barrier_index < graph.compiled_plan.barriers.size();
                 ++barrier_index) {
                auto& barrier = graph.compiled_plan.barriers[barrier_index];
                if (barrier.resource.index == candidate.resource_index &&
                    barrier.dst_pass == candidate.first_access->pass &&
                    barrier.range == candidate.first_access->range &&
                    !barrier.src_pass.IsValid() && barrier.sources.empty() &&
                    barrier.before_state.IsUndefined() &&
                    barrier.after_state == candidate.first_access->state &&
                    barrier.after_access == candidate.first_access->mode &&
                    !barrier.import_boundary && !barrier.export_boundary) {
                    alias_barrier       = &barrier;
                    alias_barrier_index = barrier_index;
                    break;
                }
            }
        }

        if (selected_slot == nullptr || alias_barrier == nullptr ||
            predecessor_cell == nullptr || primary_source == nullptr) {
            compiled_resource.transient_slot = next_slot;
            if (candidate.reusable) {
                reusable_slots.push_back(AliasSlot{
                    .id              = next_slot,
                    .tail_resource   = candidate.resource_index,
                    .available_after = compiled_resource.last_use,
                });
            }
            ++next_slot;
            continue;
        }

        const auto predecessor_resource = selected_slot->tail_resource;
        alias_barrier->src_pass       = primary_source->pass;
        alias_barrier->before_state   = predecessor_cell->state;
        alias_barrier->before_access  = primary_source->access;
        alias_barrier->src_domain     = primary_source->domain;
        alias_barrier->state_transition =
            !StatesCompatible(predecessor_cell->state, candidate.first_access->state);
        alias_barrier->memory_dependency         = true;
        alias_barrier->execution_dependency      = true;
        alias_barrier->queue_dependency          = false;
        alias_barrier->queue_ownership           = false;
        alias_barrier->discard_previous_contents = false;
        alias_barrier->source_state_unknown      = false;
        alias_barrier->transient_alias           = true;
        alias_barrier->sources                    = alias_sources;

        compiled_resource.transient_slot = selected_slot->id;
        graph.compiled_plan.alias_boundaries.push_back(
            RenderGraph::CompiledAliasBoundary{
                .transient_slot       = selected_slot->id,
                .predecessor_resource = RenderGraph::ResourceHandle{
                    predecessor_resource, graph.graph_id
                },
                .successor_resource = RenderGraph::ResourceHandle{
                    candidate.resource_index, graph.graph_id
                },
                .primary_src_pass = primary_source->pass,
                .source_frontier = [&] {
                    std::vector<RenderGraph::PassHandle> result{};
                    result.reserve(alias_sources.size());
                    for (const auto& source : alias_sources) {
                        result.push_back(source.pass);
                    }
                    return result;
                }(),
                .dst_pass      = candidate.first_access->pass,
                .barrier_index = alias_barrier_index,
            }
        );
        for (const auto& source : alias_sources) {
            assert(
                execution_position[source.pass.index] <
                compiled_resource.first_use
            );
            AddEdge(
                source.pass.index,
                candidate.first_access->pass.index,
                RenderGraph::CompiledEdgeReason{
                    .kind = RenderGraph::EdgeReasonKind::TransientAlias,
                    .resource = RenderGraph::ResourceHandle{
                        candidate.resource_index, graph.graph_id
                    },
                    .range          = candidate.first_access->range,
                    .input_version  = candidate.first_access->input_version,
                    .output_version = candidate.first_access->output_version,
                }
            );
        }

        selected_slot->tail_resource   = candidate.resource_index;
        selected_slot->available_after = compiled_resource.last_use;
    }

    // Alias edges are added after the semantic topological audit. They always
    // point forward in the immutable execution order, so the existing order
    // remains valid; restore canonical edge/reason order for all consumers.
    std::sort(
        graph.compiled_plan.edges.begin(),
        graph.compiled_plan.edges.end(),
        [](const RenderGraph::CompiledEdge& lhs, const RenderGraph::CompiledEdge& rhs) {
            return lhs.src.index < rhs.src.index ||
                   (lhs.src.index == rhs.src.index && lhs.dst.index < rhs.dst.index);
        }
    );
    for (auto& edge : graph.compiled_plan.edges) {
        std::sort(edge.reasons.begin(), edge.reasons.end(), ReasonLess);
    }
}

bool RenderGraphCompiler::ValidateTransientAliasPlan() {
    const auto& plan = graph.compiled_plan;
    if (plan.resources.size() != graph.resources.size()) {
        return Fail("transient alias plan has an incomplete resource table");
    }

    std::vector<uint32_t> execution_position(
        graph.passes.size(),
        RenderGraph::PassHandle::InvalidIndex
    );
    std::vector<uint32_t> topological_position(
        graph.passes.size(),
        RenderGraph::PassHandle::InvalidIndex
    );
    for (uint32_t position = 0; position < plan.execution_order.size(); ++position) {
        execution_position[plan.execution_order[position].index] = position;
    }
    for (uint32_t position = 0; position < plan.topological_order.size(); ++position) {
        topological_position[plan.topological_order[position].index] = position;
    }

    const auto descriptors_match = [&](uint32_t lhs_index, uint32_t rhs_index) {
        const auto& lhs = graph.resources[lhs_index];
        const auto& rhs = graph.resources[rhs_index];
        if (lhs.kind != rhs.kind) {
            return false;
        }
        if (lhs.kind == ResourceKind::Texture) {
            return lhs.transient_texture_desc.has_value() &&
                   rhs.transient_texture_desc.has_value() &&
                   *lhs.transient_texture_desc == *rhs.transient_texture_desc;
        }
        if (lhs.kind == ResourceKind::Buffer) {
            return lhs.transient_buffer_desc.has_value() &&
                   rhs.transient_buffer_desc.has_value() &&
                   *lhs.transient_buffer_desc == *rhs.transient_buffer_desc;
        }
        return false;
    };

    const auto has_alias_edge =
        [&](RenderGraph::PassHandle source,
            RenderGraph::PassHandle destination,
            RenderGraph::ResourceHandle resource,
            const RenderGraph::ResourceRange& range) {
            return std::any_of(
                plan.edges.begin(),
                plan.edges.end(),
                [&](const RenderGraph::CompiledEdge& edge) {
                    return edge.src == source && edge.dst == destination &&
                           std::any_of(
                               edge.reasons.begin(),
                               edge.reasons.end(),
                               [&](const RenderGraph::CompiledEdgeReason& reason) {
                                   return reason.kind ==
                                              RenderGraph::EdgeReasonKind::TransientAlias &&
                                          reason.resource == resource &&
                                          reason.range == range;
                               }
                           );
                }
            );
        };

    for (const auto& edge : plan.edges) {
        if (!graph.IsValidPass(edge.src) || !graph.IsValidPass(edge.dst) ||
            topological_position[edge.src.index] ==
                RenderGraph::PassHandle::InvalidIndex ||
            topological_position[edge.dst.index] ==
                RenderGraph::PassHandle::InvalidIndex ||
            topological_position[edge.src.index] >=
                topological_position[edge.dst.index]) {
            return Fail(
                "transient alias planning invalidated the compiled topological order"
            );
        }
    }

    std::vector<uint32_t> alias_barrier_owners(plan.barriers.size(), 0);
    for (const auto& alias : plan.alias_boundaries) {
        if (!graph.IsValidResource(alias.predecessor_resource) ||
            !graph.IsValidResource(alias.successor_resource) ||
            !graph.IsValidPass(alias.primary_src_pass) ||
            !graph.IsValidPass(alias.dst_pass) ||
            alias.barrier_index >= plan.barriers.size() ||
            alias.source_frontier.empty()) {
            return Fail("transient alias boundary contains an invalid handle or index");
        }

        const uint32_t predecessor_index = alias.predecessor_resource.index;
        const uint32_t successor_index   = alias.successor_resource.index;
        const auto& predecessor = plan.resources[predecessor_index];
        const auto& successor   = plan.resources[successor_index];
        const auto& predecessor_declaration = graph.resources[predecessor_index];
        const auto& successor_declaration   = graph.resources[successor_index];
        if (predecessor.transient_slot == RenderGraph::PassHandle::InvalidIndex ||
            predecessor.transient_slot != alias.transient_slot ||
            successor.transient_slot != alias.transient_slot ||
            predecessor.first_use == RenderGraph::PassHandle::InvalidIndex ||
            predecessor.last_use == RenderGraph::PassHandle::InvalidIndex ||
            successor.first_use == RenderGraph::PassHandle::InvalidIndex ||
            successor.last_use == RenderGraph::PassHandle::InvalidIndex ||
            predecessor.last_use >= successor.first_use ||
            predecessor_declaration.imported ||
            successor_declaration.imported ||
            predecessor_declaration.exported ||
            successor_declaration.exported ||
            !descriptors_match(predecessor_index, successor_index)) {
            return Fail("transient alias boundary violates slot or lifetime compatibility");
        }

        const auto& barrier = plan.barriers[alias.barrier_index];
        ++alias_barrier_owners[alias.barrier_index];
        if (!barrier.transient_alias ||
            barrier.resource != alias.successor_resource ||
            barrier.src_pass != alias.primary_src_pass ||
            barrier.dst_pass != alias.dst_pass ||
            !barrier.memory_dependency || !barrier.execution_dependency ||
            barrier.queue_dependency || barrier.queue_ownership ||
            barrier.discard_previous_contents || barrier.import_boundary ||
            barrier.export_boundary || barrier.source_state_unknown ||
            barrier.sources.size() != alias.source_frontier.size()) {
            return Fail("transient alias boundary and barrier metadata disagree");
        }

        for (uint32_t source_index = 0;
             source_index < alias.source_frontier.size();
             ++source_index) {
            const auto source = alias.source_frontier[source_index];
            if (!graph.IsValidPass(source) ||
                barrier.sources[source_index].pass != source ||
                execution_position[source.index] ==
                    RenderGraph::PassHandle::InvalidIndex ||
                execution_position[alias.dst_pass.index] ==
                    RenderGraph::PassHandle::InvalidIndex ||
                execution_position[source.index] >=
                    execution_position[alias.dst_pass.index] ||
                !has_alias_edge(
                    source,
                    alias.dst_pass,
                    alias.successor_resource,
                    barrier.range
                )) {
                return Fail(
                    "transient alias source frontier is not fully ordered before its successor"
                );
            }
        }
        if (alias.source_frontier.back() != alias.primary_src_pass) {
            return Fail("transient alias primary source is not the latest frontier source");
        }
    }

    for (uint32_t barrier_index = 0; barrier_index < plan.barriers.size();
         ++barrier_index) {
        const uint32_t owner_count = alias_barrier_owners[barrier_index];
        if ((plan.barriers[barrier_index].transient_alias && owner_count != 1) ||
            (!plan.barriers[barrier_index].transient_alias && owner_count != 0)) {
            return Fail(
                "transient alias barrier does not have exactly one boundary owner"
            );
        }
    }

    uint32_t max_slot = 0;
    bool     has_slot = false;
    for (const auto& resource : plan.resources) {
        if (resource.transient_slot != RenderGraph::PassHandle::InvalidIndex) {
            max_slot = std::max(max_slot, resource.transient_slot);
            has_slot = true;
        }
    }
    for (uint32_t slot = 0; has_slot && slot <= max_slot; ++slot) {
        std::vector<uint32_t> occupants{};
        for (uint32_t resource_index = 0; resource_index < plan.resources.size();
             ++resource_index) {
            if (plan.resources[resource_index].transient_slot == slot) {
                occupants.push_back(resource_index);
            }
        }
        if (occupants.empty()) {
            continue;
        }
        std::sort(
            occupants.begin(),
            occupants.end(),
            [&](uint32_t lhs, uint32_t rhs) {
                const auto& lhs_resource = plan.resources[lhs];
                const auto& rhs_resource = plan.resources[rhs];
                return lhs_resource.first_use < rhs_resource.first_use ||
                       (lhs_resource.first_use == rhs_resource.first_use &&
                        lhs < rhs);
            }
        );
        for (uint32_t index = 0; index < occupants.size(); ++index) {
            const auto resource_index = occupants[index];
            const auto& resource      = plan.resources[resource_index];
            if (resource.first_use == RenderGraph::PassHandle::InvalidIndex ||
                resource.last_use == RenderGraph::PassHandle::InvalidIndex ||
                graph.resources[resource_index].imported) {
                return Fail("transient slot contains an inactive or imported resource");
            }
            if (index == 0) {
                continue;
            }
            const uint32_t predecessor_index = occupants[index - 1];
            const auto& predecessor = plan.resources[predecessor_index];
            if (predecessor.last_use >= resource.first_use ||
                !descriptors_match(predecessor_index, resource_index)) {
                return Fail(
                    "transient slot occupants overlap or have incompatible descriptors"
                );
            }
            const bool has_boundary = std::any_of(
                plan.alias_boundaries.begin(),
                plan.alias_boundaries.end(),
                [&](const RenderGraph::CompiledAliasBoundary& alias) {
                    return alias.transient_slot == slot &&
                           alias.predecessor_resource.index ==
                               predecessor_index &&
                           alias.successor_resource.index == resource_index;
                }
            );
            if (!has_boundary) {
                return Fail(
                    "adjacent transient slot occupants lack an alias boundary"
                );
            }
        }
    }

    return true;
}

bool RenderGraphCompiler::RangesOverlap(const ResourceRange& lhs, const ResourceRange& rhs) {
    if (lhs.kind != rhs.kind) {
        return false;
    }
    switch (lhs.kind) {
        case ResourceKind::Texture: {
            if ((AspectMask(lhs.texture.aspects) & AspectMask(rhs.texture.aspects)) == 0) {
                return false;
            }
            const uint32_t lhs_mip_end   = lhs.texture.mip_first + lhs.texture.mip_count;
            const uint32_t rhs_mip_end   = rhs.texture.mip_first + rhs.texture.mip_count;
            const uint32_t lhs_layer_end = lhs.texture.layer_first + lhs.texture.layer_count;
            const uint32_t rhs_layer_end = rhs.texture.layer_first + rhs.texture.layer_count;
            return lhs.texture.mip_first < rhs_mip_end && rhs.texture.mip_first < lhs_mip_end &&
                   lhs.texture.layer_first < rhs_layer_end && rhs.texture.layer_first < lhs_layer_end;
        }
        case ResourceKind::Buffer:
            if (lhs.buffer.size == RenderGraph::RemainingBufferRange ||
                rhs.buffer.size == RenderGraph::RemainingBufferRange) {
                return true;
            }
            return lhs.buffer.offset < rhs.buffer.offset + rhs.buffer.size &&
                   rhs.buffer.offset < lhs.buffer.offset + lhs.buffer.size;
        case ResourceKind::Token:
            return true;
    }
    return false;
}

bool RenderGraphCompiler::Fail(std::string message) {
    return graph.FailCompile(std::move(message));
}

} // namespace Moer::Render
