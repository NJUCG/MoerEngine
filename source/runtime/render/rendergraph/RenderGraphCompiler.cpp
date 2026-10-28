#include "rendergraph/RenderGraphCompiler.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <functional>
#include <queue>
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

template<typename T, typename Allocator>
void SortUnique(std::vector<T, Allocator>& values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

[[nodiscard]] auto OutgoingEdges(const std::vector<RenderGraph::CompiledEdge>& edges, uint32_t pass_index) {
    // Both callers consume the canonical edge order (source, then destination).
    return std::ranges::equal_range(edges, pass_index, {}, [](const auto& edge) {
        return edge.src.pass_id;
    });
}

[[nodiscard]] auto RangeSortKey(const ResourceRange& range) {
    // Keep resource-kind ordering independent of the variant's alternative order.
    return std::tuple<ResourceKind, const ResourceRange&>{RenderGraph::GetRangeKind(range), range};
}

[[nodiscard]] bool
ReasonLess(const RenderGraph::CompiledEdgeReason& lhs, const RenderGraph::CompiledEdgeReason& rhs) {
    const auto lhs_key =
        std::tuple{static_cast<uint8_t>(lhs.kind), lhs.resource.resource_id, lhs.input_version, lhs.output_version};
    const auto rhs_key =
        std::tuple{static_cast<uint8_t>(rhs.kind), rhs.resource.resource_id, rhs.input_version, rhs.output_version};
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

[[nodiscard]] bool TextureAccessSupports(RenderGraph::TextureAccessType type, RenderGraph::AccessMode mode) {
    using Type = RenderGraph::TextureAccessType;
    using Mode = RenderGraph::AccessMode;
    if (mode == Mode::Unknown || type == Type::Unspecified) {
        return true;
    }
    switch (type) {
        case Type::TransferSource:
        case Type::Sampled:
            return mode == Mode::Read;
        case Type::TransferDestination:
            return mode == Mode::Write;
        case Type::ColorAttachment:
            return mode == Mode::Write || mode == Mode::ReadWrite;
        case Type::Storage:
        case Type::DepthStencilAttachment:
            return mode == Mode::Read || mode == Mode::Write || mode == Mode::ReadWrite;
        case Type::Unspecified:
            return true;
    }
    return false;
}

[[nodiscard]] bool TextureAccessSupportsAspects(RenderGraph::TextureAccessType type, TextureAspect aspects) {
    using Type             = RenderGraph::TextureAccessType;
    const uint8_t selected = AspectMask(aspects);
    switch (type) {
        case Type::ColorAttachment:
        case Type::Storage:
            return selected == AspectMask(TextureAspect::Color);
        case Type::DepthStencilAttachment: {
            const uint8_t depth_stencil = AspectMask(TextureAspect::Depth) | AspectMask(TextureAspect::Stencil);
            return selected != 0 && (selected & depth_stencil) == selected;
        }
        default:
            return true;
    }
}

[[nodiscard]] bool BufferAccessSupports(RenderGraph::BufferAccessType type, RenderGraph::AccessMode mode) {
    using Type = RenderGraph::BufferAccessType;
    using Mode = RenderGraph::AccessMode;
    if (mode == Mode::Unknown || type == Type::Unspecified) {
        return true;
    }
    switch (type) {
        case Type::TransferSource:
        case Type::VertexBuffer:
        case Type::IndexBuffer:
        case Type::IndirectArgument:
        case Type::ShaderRead:
        case Type::AccelerationStructureBuildInput:
        case Type::AccelerationStructureRead:
            return mode == Mode::Read;
        case Type::TransferDestination:
        case Type::AccelerationStructureWrite:
            return mode == Mode::Write;
        case Type::Storage:
            return mode == Mode::Read || mode == Mode::Write || mode == Mode::ReadWrite;
        case Type::Unspecified:
            return true;
    }
    return false;
}

[[nodiscard]] bool BoundarySupportsAccess(const RenderGraph::ResourceState& state, RenderGraph::AccessMode mode) {
    if (const auto* texture = std::get_if<RenderGraph::TextureAccessType>(&state)) {
        return TextureAccessSupports(*texture, mode);
    }
    if (const auto* buffer = std::get_if<RenderGraph::BufferAccessType>(&state)) {
        return BufferAccessSupports(*buffer, mode);
    }
    if (RenderGraph::IsUndefined(state)) {
        return mode == RenderGraph::AccessMode::None;
    }
    return RenderGraph::IsPresentationSource(state) &&
           (mode == RenderGraph::AccessMode::Read || mode == RenderGraph::AccessMode::Unknown);
}

[[nodiscard]] bool TextureBoundarySupportsAspects(const RenderGraph::ResourceState& state, TextureAspect aspects) {
    if (const auto* type = std::get_if<RenderGraph::TextureAccessType>(&state)) {
        return TextureAccessSupportsAspects(*type, aspects);
    }
    return RenderGraph::IsUndefined(state) ||
           (RenderGraph::IsPresentationSource(state) && aspects == TextureAspect::Color);
}

[[nodiscard]] bool StatesCompatible(
    const RenderGraph::ResourceState& previous,
    const RenderGraph::ResourceState& next
) {
    return previous == next && !RenderGraph::IsUnspecified(previous);
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
               resource.texture_desc.sharing_mode == RenderGraph::SharingMode::Exclusive :
           resource.kind == ResourceKind::Buffer ?
               resource.buffer_desc.sharing_mode == RenderGraph::SharingMode::Exclusive :
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
    edge_indices.clear();
    for (auto& resource : graph.resources) {
        resource.first_use = RenderGraph::InvalidIndex;
        resource.last_use  = RenderGraph::InvalidIndex;
    }

    if (!graph.declaration_errors.empty()) {
        return Fail(graph.declaration_errors.front());
    }
    if (graph.passes.empty()) {
        return Fail("graph contains no passes");
    }

    normalized_accesses.clear();
    normalized_accesses.resize(graph.passes.size());
    normalized_initial_states.clear();
    normalized_initial_states.resize(graph.resources.size());
    normalized_final_states.clear();
    normalized_final_states.resize(graph.resources.size());
    resource_cells.clear();
    resource_cells.resize(graph.resources.size());
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
    BuildDependencyLevels();
    BuildFrontendRecordUnits();
    BuildFrontendDispatchGroups();
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
        normalized_accesses[pass_index].reserve(pass.accesses.size());
        if (!pass.record) {
            return Fail("pass '" + pass.name + "' has no record callback");
        }
        if (pass.record_policy != RenderGraph::PassRecordPolicy::Serial &&
            pass.record_policy != RenderGraph::PassRecordPolicy::ParallelAllowed) {
            return Fail("pass '" + pass.name + "' has an invalid recording policy");
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
            const auto&   resource = graph.resources[access.resource.resource_id];
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
                                    std::pmr::vector<NormalizedStateDeclaration>&     normalized_states,
                                    bool                                              initial
                                ) -> bool {
            for (const auto& declaration : declarations) {
                if (RenderGraph::IsUnspecified(declaration.state)) {
                    return Fail("boundary state cannot be unspecified on resource '" + resource.name + "'");
                }
                const bool presentation_source = RenderGraph::IsPresentationSource(declaration.state);
                if (presentation_source &&
                    declaration.queue != RenderGraph::QueueRole::Graphics) {
                    return Fail(
                        "PresentationSource boundary requires the Graphics queue on resource '" +
                        resource.name + "'"
                    );
                }
                if (declaration.queue == RenderGraph::QueueRole::None &&
                    !RenderGraph::IsUndefined(declaration.state)) {
                    return Fail("a known boundary state requires an owner queue on resource '" + resource.name + "'");
                }
                if (!initial && RenderGraph::IsUndefined(declaration.state)) {
                    return Fail("an exported resource cannot require the undefined state: " + resource.name);
                }
                if (initial && !RenderGraph::IsUndefined(declaration.state) &&
                    declaration.boundary_access == RenderGraph::AccessMode::None) {
                    return Fail("a known imported state must describe its previous access on resource '" +
                                resource.name + "'");
                }
                if (initial && RenderGraph::IsUndefined(declaration.state) &&
                    declaration.boundary_access != RenderGraph::AccessMode::None) {
                    return Fail("an undefined imported state must have no previous access on resource '" +
                                resource.name + "'");
                }
                if (declaration.boundary_access != RenderGraph::AccessMode::None) {
                    const bool supported = BoundarySupportsAccess(declaration.state, declaration.boundary_access);
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
                    !TextureBoundarySupportsAspects(
                        declaration.state, std::get<RenderGraph::TextureRange>(normalized).aspects
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
    if (RenderGraph::GetRangeKind(requested) != resource.kind) {
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

            auto& range = std::get<RenderGraph::TextureRange>(normalized);
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
            auto& range = std::get<RenderGraph::BufferRange>(normalized);
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
            normalized = std::monostate{};
            break;
    }
    return true;
}

bool RenderGraphCompiler::ValidateExecutionDomain(uint32_t pass_index) {
    const auto& pass   = graph.passes[pass_index];
    const auto  queue  = pass.domain.queue;
    const auto  work   = pass.domain.work;
    if (queue == RenderGraph::QueueRole::None || work == RenderGraph::GpuWorkType::None) {
        return Fail("pass '" + pass.name + "' has an incomplete execution domain");
    }
    if (!graph.queue_topology.Resolve(queue).available) {
        return Fail(
            "pass '" + pass.name + "' targets an unavailable logical queue"
        );
    }
    if (queue == RenderGraph::QueueRole::Copy && work != RenderGraph::GpuWorkType::Transfer) {
        return Fail("copy queue pass '" + pass.name + "' must declare Transfer work");
    }
    if (work == RenderGraph::GpuWorkType::Graphics &&
        queue != RenderGraph::QueueRole::Graphics) {
        return Fail("graphics pass '" + pass.name + "' must use the graphics queue");
    }
    if (work == RenderGraph::GpuWorkType::RayTracing &&
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
    if (!explicit_state || RenderGraph::IsUnspecified(state)) {
        if (explicit_state) {
            return Fail("pass '" + graph.passes[pass_index].name + "' explicitly requested the unspecified state");
        }
        return true;
    }

    const auto& pass = graph.passes[pass_index];
    bool        supported = false;
    bool        transfer_state = false;
    if (resource.kind == ResourceKind::Texture) {
        const auto texture = std::get<RenderGraph::TextureAccessType>(state);
        supported          = TextureAccessSupports(texture, mode);
        transfer_state     = texture == RenderGraph::TextureAccessType::TransferSource ||
                         texture == RenderGraph::TextureAccessType::TransferDestination;
        const bool attachment_state = texture == RenderGraph::TextureAccessType::ColorAttachment ||
                                      texture == RenderGraph::TextureAccessType::DepthStencilAttachment;
        if (attachment_state && pass.domain.work != RenderGraph::GpuWorkType::Graphics) {
            return Fail("attachment state requires Graphics work in pass '" + pass.name + "'");
        }
        if (!TextureAccessSupportsAspects(texture, std::get<RenderGraph::TextureRange>(range).aspects)) {
            return Fail(
                "texture state is incompatible with the selected aspects in pass '" + pass.name + "'"
            );
        }
    } else {
        const auto buffer = std::get<RenderGraph::BufferAccessType>(state);
        supported         = BufferAccessSupports(buffer, mode);
        transfer_state    = buffer == RenderGraph::BufferAccessType::TransferSource ||
                         buffer == RenderGraph::BufferAccessType::TransferDestination;
        const bool graphics_input = buffer == RenderGraph::BufferAccessType::VertexBuffer ||
                                    buffer == RenderGraph::BufferAccessType::IndexBuffer;
        if (graphics_input && pass.domain.work != RenderGraph::GpuWorkType::Graphics) {
            return Fail("vertex/index state requires Graphics work in pass '" + pass.name + "'");
        }
    }
    if (!supported) {
        return Fail("access mode is incompatible with the explicit resource state in pass '" +
                    pass.name + "'");
    }
    if ((pass.domain.queue == RenderGraph::QueueRole::Copy ||
         pass.domain.work == RenderGraph::GpuWorkType::Transfer) &&
        !transfer_state) {
        return Fail("Transfer work or a copy queue requires a transfer state in pass '" + pass.name + "'");
    }
    return true;
}

bool RenderGraphCompiler::BuildAtomicCells() {
    static constexpr std::array<TextureAspect, 3> texture_aspects{
        TextureAspect::Color, TextureAspect::Depth, TextureAspect::Stencil
    };

    std::pmr::vector<std::pmr::vector<const ResourceRange*>> ranges_by_resource{&scratch_memory};
    ranges_by_resource.resize(graph.resources.size());
    for (const auto& accesses : normalized_accesses) {
        for (const auto& access : accesses) {
            ranges_by_resource[access.resource.resource_id].push_back(&access.range);
        }
    }

    // Endpoints are temporary; reuse their capacity across resources.
    std::pmr::vector<uint32_t> mip_boundaries{&scratch_memory};
    std::pmr::vector<uint32_t> layer_boundaries{&scratch_memory};
    std::pmr::vector<uint64_t> boundaries{&scratch_memory};
    for (uint32_t resource_index = 0; resource_index < graph.resources.size(); ++resource_index) {
        const auto& resource = graph.resources[resource_index];
        auto&       cells    = resource_cells[resource_index];

        if (resource.kind == ResourceKind::Texture) {
            mip_boundaries.assign({0, resource.texture_desc.mip_count});
            layer_boundaries.assign({0, resource.texture_desc.layer_count});
            auto add_texture_boundaries = [&](const ResourceRange& range) {
                const auto& texture = std::get<RenderGraph::TextureRange>(range);
                if (texture.mip_first != 0) {
                    mip_boundaries.push_back(texture.mip_first);
                }
                if (texture.mip_first + texture.mip_count != resource.texture_desc.mip_count) {
                    mip_boundaries.push_back(texture.mip_first + texture.mip_count);
                }
                if (texture.layer_first != 0) {
                    layer_boundaries.push_back(texture.layer_first);
                }
                if (texture.layer_first + texture.layer_count != resource.texture_desc.layer_count) {
                    layer_boundaries.push_back(texture.layer_first + texture.layer_count);
                }
            };
            for (const auto* range : ranges_by_resource[resource_index]) {
                add_texture_boundaries(*range);
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
                        cell.range       = RenderGraph::TextureRange{
                            .aspects     = aspect,
                            .mip_first   = mip_first,
                            .mip_count   = mip_end - mip_first,
                            .layer_first = layer_first,
                            .layer_count = layer_end - layer_first
                        };
                        cell.initialized = resource.imported;
                        cell.version     = resource.imported ? 0 : RenderGraph::InvalidVersion;
                        cell.state       = resource.imported ? RenderGraph::ResourceState{RenderGraph::TextureAccessType::Unspecified} :
                                                               RenderGraph::ResourceState{RenderGraph::TextureBoundary::Undefined};
                        cell.state_known = !resource.imported;
                        cells.push_back(std::move(cell));
                    }
                }
            }
        } else if (resource.kind == ResourceKind::Buffer && resource.buffer_desc.byte_size != 0) {
            boundaries.assign({0, resource.buffer_desc.byte_size});
            auto add_buffer_boundaries = [&](const ResourceRange& range) {
                const auto& buffer = std::get<RenderGraph::BufferRange>(range);
                if (buffer.offset != 0) {
                    boundaries.push_back(buffer.offset);
                }
                if (buffer.offset + buffer.size != resource.buffer_desc.byte_size) {
                    boundaries.push_back(buffer.offset + buffer.size);
                }
            };
            for (const auto* range : ranges_by_resource[resource_index]) {
                add_buffer_boundaries(*range);
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
                cell.range       = RenderGraph::BufferRange{
                    .offset = boundaries[index], .size = boundaries[index + 1] - boundaries[index]
                };
                cell.initialized = resource.imported;
                cell.version     = resource.imported ? 0 : RenderGraph::InvalidVersion;
                cell.state       = resource.imported ? RenderGraph::ResourceState{RenderGraph::BufferAccessType::Unspecified} :
                                                       RenderGraph::ResourceState{RenderGraph::BufferBoundary::Undefined};
                cell.state_known = !resource.imported;
                cells.push_back(std::move(cell));
            }
        } else {
            AtomicCell cell{};
            cell.range       = RenderGraph::WholeResourceRange(resource.kind);
            cell.initialized = resource.imported;
            cell.version     = resource.imported ? 0 : RenderGraph::InvalidVersion;
            if (resource.kind == ResourceKind::Buffer) {
                cell.state = resource.imported ? RenderGraph::ResourceState{RenderGraph::BufferAccessType::Unspecified} :
                                                 RenderGraph::ResourceState{RenderGraph::BufferBoundary::Undefined};
            } else {
                cell.state = RenderGraph::ResourceState{};
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
                cell.initialized                 = !RenderGraph::IsUndefined(initial.state);
                if (!cell.initialized) {
                    cell.version = RenderGraph::InvalidVersion;
                }
                cell.last_domain                 = RenderGraph::ExecutionDomain{
                    initial.queue, RenderGraph::GpuWorkType::None
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
    // Reuse cell storage across passes, and visit only resources that the
    // current pass accesses. Unused resources have no per-pass state to merge.
    std::pmr::vector<std::pmr::vector<CellUsage>> usages{&scratch_memory};
    usages.resize(graph.resources.size());
    std::pmr::vector<uint32_t> accessed_resources{&scratch_memory};
    for (uint32_t pass_index = 0; pass_index < graph.passes.size(); ++pass_index) {
        const auto& pass = graph.passes[pass_index];
        for (const auto dependency : pass.explicit_dependencies) {
            if (!graph.IsValidPass(dependency)) {
                return Fail("pass '" + pass.name + "' has an invalid explicit dependency");
            }
            AddEdge(
                dependency.pass_id,
                pass_index,
                RenderGraph::CompiledEdgeReason{.kind = RenderGraph::EdgeReasonKind::Explicit}
            );
        }

        accessed_resources.clear();
        for (const auto& access : normalized_accesses[pass_index]) {
            auto&       resource_usages = usages[access.resource.resource_id];
            const auto& cells           = resource_cells[access.resource.resource_id];
            if (resource_usages.empty()) {
                resource_usages.resize(cells.size());
                accessed_resources.push_back(access.resource.resource_id);
            }
            bool        matched         = false;
            for (uint32_t cell_index = 0; cell_index < cells.size(); ++cell_index) {
                if (!RangesOverlap(access.range, cells[cell_index].range)) {
                    continue;
                }
                matched     = true;
                auto& usage = resource_usages[cell_index];
                if (!MergeCellState(
                        pass_index, graph.resources[access.resource.resource_id], usage, access
                    )) {
                    return false;
                }
            }
            if (!matched) {
                return Fail("access range did not map to a compiler interval in pass '" + pass.name + "'");
            }
        }

        // Keep the compiler's deterministic resource order independent of the
        // order in which a pass declared its accesses.
        std::sort(accessed_resources.begin(), accessed_resources.end());
        for (const uint32_t resource_index : accessed_resources) {
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
                                                     RenderGraph::UnspecifiedResourceState(resource.kind);

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
                if (usage.read && cell.last_writer != RenderGraph::InvalidIndex) {
                    append_source(cell.last_writer_source);
                }
                if (usage.write) {
                    if (cell.last_writer != RenderGraph::InvalidIndex) {
                        append_source(cell.last_writer_source);
                    }
                    for (const auto& reader : cell.readers) {
                        append_source(reader);
                    }
                }

                const bool boundary_memory_dependency =
                    cell.last_access == RenderGraph::InvalidIndex &&
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
                    if (cell.last_access != RenderGraph::InvalidIndex &&
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
                                source.pass.pass_id,
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
                                source.pass.pass_id,
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
                    cell.availability_established_pass != RenderGraph::InvalidIndex &&
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
                    cell.ownership_established_pass != RenderGraph::InvalidIndex &&
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
                                             cell.last_access == RenderGraph::InvalidIndex;
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
                    if (cell.last_writer != RenderGraph::InvalidIndex) {
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
                    if (cell.last_writer != RenderGraph::InvalidIndex) {
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
                            reader.pass.pass_id,
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
                        cell.last_writer        = RenderGraph::InvalidIndex;
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
                    // state/data availability for later accesses. Unspecified accesses still inherit
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
            resource_usages.clear();
        }
    }

    for (uint32_t resource_index = 0; resource_index < graph.resources.size(); ++resource_index) {
        const auto& resource = graph.resources[resource_index];
        if (!resource.imported && !resource_ever_written[resource_index]) {
            const auto& cells = resource_cells[resource_index];
            const bool has_gpu_access = std::any_of(cells.begin(), cells.end(), [](const AtomicCell& cell) {
                return cell.last_access != RenderGraph::InvalidIndex;
            });
            const bool has_opaque_reference = std::any_of(
                graph.passes.begin(),
                graph.passes.end(),
                [&](const RenderGraph::PassDeclaration& pass) {
                    return std::any_of(
                        pass.references.begin(),
                        pass.references.end(),
                        [&](RenderGraph::ResourceHandle reference) {
                            return reference.resource_id == resource_index;
                        }
                    );
                }
            );
            if (has_gpu_access || has_opaque_reference) {
                return Fail("transient resource has no producer: " + resource.name);
            }
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
        const bool supported     = resource.kind == ResourceKind::Texture ?
                                       TextureAccessSupports(std::get<RenderGraph::TextureAccessType>(usage.state), combined_mode) :
                                   resource.kind == ResourceKind::Buffer ?
                                       BufferAccessSupports(std::get<RenderGraph::BufferAccessType>(usage.state), combined_mode) :
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
            return lhs.pass.pass_id < rhs.pass.pass_id;
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
        .discard_previous_contents = cell.state_known && RenderGraph::IsUndefined(cell.state),
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
                if (sources.empty() && cell.last_access != RenderGraph::InvalidIndex) {
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
                RenderGraph::ExecutionDomain{final->queue, RenderGraph::GpuWorkType::None},
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
                                              cell.last_access != RenderGraph::InvalidIndex;
            if (participates_in_plan && !cell.state_known) {
                graph.compiled_plan.state_plan_complete = false;
                return;
            }
        }
    }
}

void RenderGraphCompiler::AddEdge(uint32_t src, uint32_t dst, RenderGraph::CompiledEdgeReason reason) {
    if (src == dst || src == RenderGraph::InvalidIndex ||
        dst == RenderGraph::InvalidIndex) {
        return;
    }

    const auto src_handle = RenderGraph::PassHandle{src, graph.graph_id};
    const auto dst_handle = RenderGraph::PassHandle{dst, graph.graph_id};
    const uint64_t key = (uint64_t{src} << 32) | dst;
    const auto found = edge_indices.find(key);
    if (found == edge_indices.end()) {
        const size_t index = graph.compiled_plan.edges.size();
        graph.compiled_plan.edges.push_back(
            RenderGraph::CompiledEdge{src_handle, dst_handle, {std::move(reason)}}
        );
        edge_indices.emplace(key, index);
        return;
    }
    auto& reasons = graph.compiled_plan.edges[found->second].reasons;
    // Consumers only read reasons after SortEdges canonicalizes the plan.
    reasons.push_back(std::move(reason));
}

void RenderGraphCompiler::SortEdges() {
    std::sort(
        graph.compiled_plan.edges.begin(),
        graph.compiled_plan.edges.end(),
        [](const RenderGraph::CompiledEdge& lhs, const RenderGraph::CompiledEdge& rhs) {
            return lhs.src.pass_id < rhs.src.pass_id ||
                   (lhs.src.pass_id == rhs.src.pass_id && lhs.dst.pass_id < rhs.dst.pass_id);
        }
    );
    // Alias planning may add edges after the semantic sort. Keep the lookup
    // synchronized with the canonical output order at both sort boundaries.
    for (size_t index = 0; index < graph.compiled_plan.edges.size(); ++index) {
        auto& edge = graph.compiled_plan.edges[index];
        std::sort(edge.reasons.begin(), edge.reasons.end(), ReasonLess);
        edge.reasons.erase(std::unique(edge.reasons.begin(), edge.reasons.end()), edge.reasons.end());
        edge_indices.at((uint64_t{edge.src.pass_id} << 32) | edge.dst.pass_id) = index;
    }
}

bool RenderGraphCompiler::BuildTopologicalOrder() {
    SortEdges();
    std::pmr::vector<uint32_t> indegree(graph.passes.size(), 0, &scratch_memory);
    for (const auto& edge : graph.compiled_plan.edges) {
        ++indegree[edge.dst.pass_id];
    }

    // Preserve the lowest declaration index tie-break without rescanning all passes.
    std::priority_queue<uint32_t, std::pmr::vector<uint32_t>, std::greater<>> ready{
        std::greater<>{}, std::pmr::vector<uint32_t>{&scratch_memory}
    };
    for (uint32_t pass_index = 0; pass_index < graph.passes.size(); ++pass_index) {
        if (indegree[pass_index] == 0) {
            ready.push(pass_index);
        }
    }
    graph.compiled_plan.topological_order.reserve(graph.passes.size());
    while (!ready.empty()) {
        const uint32_t next = ready.top();
        ready.pop();
        graph.compiled_plan.topological_order.push_back(RenderGraph::PassHandle{next, graph.graph_id});
        for (const auto& edge : OutgoingEdges(graph.compiled_plan.edges, next)) {
            assert(indegree[edge.dst.pass_id] > 0);
            if (--indegree[edge.dst.pass_id] == 0) {
                ready.push(edge.dst.pass_id);
            }
        }
    }
    if (graph.compiled_plan.topological_order.size() != graph.passes.size()) {
        return Fail("pass dependency cycle detected");
    }
    return true;
}

bool RenderGraphCompiler::BuildExecutionOrder() {
    graph.compiled_plan.execution_order.reserve(graph.passes.size());
    for (uint32_t pass_index = 0; pass_index < graph.passes.size(); ++pass_index) {
        graph.compiled_plan.execution_order.push_back(RenderGraph::PassHandle{pass_index, graph.graph_id});
    }

    for (const auto& edge : graph.compiled_plan.edges) {
        if (edge.src.pass_id >= edge.dst.pass_id) {
            return Fail("serial declaration order cannot satisfy a compiled dependency");
        }
    }
    return true;
}

void RenderGraphCompiler::BuildQueuePlan() {
    std::pmr::vector<uint32_t> pass_to_batch(
        graph.passes.size(), RenderGraph::InvalidIndex, &scratch_memory
    );
    for (const auto pass_handle : graph.compiled_plan.execution_order) {
        const auto& pass    = graph.passes[pass_handle.pass_id];
        const auto  binding = graph.queue_topology.Resolve(pass.domain.queue);
        if (graph.compiled_plan.queue_batches.empty() ||
            graph.compiled_plan.queue_batches.back().queue.role != binding.role) {
            const uint32_t batch_id =
                static_cast<uint32_t>(graph.compiled_plan.queue_batches.size());
            graph.compiled_plan.queue_batches.push_back(RenderGraph::CompiledQueueBatch{
                .id = batch_id,
                .queue = binding,
            });
        }
        auto& batch = graph.compiled_plan.queue_batches.back();
        batch.passes.push_back(pass_handle);
        pass_to_batch[pass_handle.pass_id] = batch.id;
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
            if (edge_index != RenderGraph::InvalidIndex &&
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
            .dependency_edges  = edge_index == RenderGraph::InvalidIndex ?
                                     std::vector<uint32_t>{} :
                                     std::vector<uint32_t>{edge_index},
        });
        producer.signal_syncs.push_back(sync_id);
        consumer.wait_syncs.push_back(sync_id);
        return graph.compiled_plan.queue_syncs.back();
    };

    for (uint32_t edge_index = 0; edge_index < graph.compiled_plan.edges.size(); ++edge_index) {
        const auto& edge = graph.compiled_plan.edges[edge_index];
        const uint32_t signal_batch = pass_to_batch[edge.src.pass_id];
        const uint32_t wait_batch   = pass_to_batch[edge.dst.pass_id];
        if (signal_batch == RenderGraph::InvalidIndex ||
            wait_batch == RenderGraph::InvalidIndex) {
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
                    RenderGraph::InvalidIndex
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
            const uint32_t dst_batch = pass_to_batch[barrier.dst_pass.pass_id];
            graph.compiled_plan.pass_barriers[barrier.dst_pass.pass_id].before.push_back(barrier_index);
            if (!barrier.src_pass.IsValid() || barrier.queue_dependency) {
                graph.compiled_plan.queue_batches[dst_batch].pre_barriers.push_back(barrier_index);
            }
        }
        auto add_post_barrier = [&](RenderGraph::PassHandle source_pass) {
            if (!source_pass.IsValid() || !barrier.dst_pass.IsValid()) {
                return;
            }
            const uint32_t src_batch = pass_to_batch[source_pass.pass_id];
            const uint32_t dst_batch = pass_to_batch[barrier.dst_pass.pass_id];
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
        const uint32_t dst_batch = pass_to_batch[barrier.dst_pass.pass_id];
        auto attach_to_sync = [&](RenderGraph::PassHandle source_pass) {
            if (!source_pass.IsValid()) {
                return;
            }
            const uint32_t src_batch = pass_to_batch[source_pass.pass_id];
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

void RenderGraphCompiler::BuildDependencyLevels() {
    // A pass enters the wave after its deepest predecessor. The validated
    // topological order also covers alias edges added after the initial sort.
    std::pmr::vector<uint32_t> levels(graph.passes.size(), 0, &scratch_memory);
    uint32_t max_level = 0;
    for (const auto pass : graph.compiled_plan.topological_order) {
        for (const auto& edge : OutgoingEdges(graph.compiled_plan.edges, pass.pass_id)) {
            auto& destination_level = levels[edge.dst.pass_id];
            destination_level = std::max(destination_level, levels[pass.pass_id] + 1);
            max_level = std::max(max_level, destination_level);
        }
    }
    graph.compiled_plan.dependency_levels.resize(max_level + 1);
    for (uint32_t pass_index = 0; pass_index < graph.passes.size(); ++pass_index) {
        graph.compiled_plan.dependency_levels[levels[pass_index]].passes.push_back(
            RenderGraph::PassHandle{pass_index, graph.graph_id}
        );
    }
}

void RenderGraphCompiler::BuildFrontendRecordUnits() {
    std::pmr::vector<uint32_t> pass_to_dependency_level(
        graph.passes.size(),
        RenderGraph::InvalidIndex,
        &scratch_memory
    );
    for (uint32_t level_index = 0;
         level_index < graph.compiled_plan.dependency_levels.size();
         ++level_index) {
        for (const auto pass :
             graph.compiled_plan.dependency_levels[level_index].passes) {
            pass_to_dependency_level[pass.pass_id] = level_index;
        }
    }

    // Preserve one stable frontend source identity per pass in execution order.
    graph.compiled_plan.frontend_record_units.reserve(
        graph.compiled_plan.execution_order.size()
    );
    for (const auto pass_handle : graph.compiled_plan.execution_order) {
        const auto& pass = graph.passes[pass_handle.pass_id];
        const auto source_index = static_cast<uint32_t>(
            graph.compiled_plan.frontend_record_units.size()
        );
        graph.compiled_plan.frontend_record_units.push_back(
            RenderGraph::CompiledFrontendRecordUnit{
                .source_index = source_index,
                .target_queue = graph.queue_topology.Resolve(pass.domain.queue),
                .pass         = pass_handle,
                .record_policy = pass.record_policy,
                .recording_workload = pass.recording_workload,
                .native_translate_class = pass.translate_execution_class,

                .dependency_level_index =
                    pass_to_dependency_level[pass_handle.pass_id],
            }
        );
    }
}

void RenderGraphCompiler::BuildFrontendDispatchGroups() {
    // Graph edges describe GPU execution, state, and ownership. They do not
    // order frontend Record callbacks: every callback consumes immutable
    // prepared data and writes a private command stream. Only the declared CPU
    // recording policy creates a frontend dispatch boundary.
    uint32_t unit_index = 0;
    while (unit_index < graph.compiled_plan.frontend_record_units.size()) {
        const auto& first =
            graph.compiled_plan.frontend_record_units[unit_index];
        uint32_t group_end = unit_index + 1;
        if (first.record_policy ==
            RenderGraph::PassRecordPolicy::ParallelAllowed) {
            while (group_end <
                   graph.compiled_plan.frontend_record_units.size()) {
                const auto& candidate =
                    graph.compiled_plan.frontend_record_units[group_end];
                if (candidate.record_policy !=
                    RenderGraph::PassRecordPolicy::ParallelAllowed) {
                    break;
                }
                ++group_end;
            }
        }

        graph.compiled_plan.frontend_dispatch_groups.push_back(
            RenderGraph::CompiledFrontendDispatchGroup{
                .first_unit = unit_index,
                .unit_count = group_end - unit_index,
                .record_policy = first.record_policy,
            }
        );
        unit_index = group_end;
    }
}

void RenderGraphCompiler::BuildLifetimes() {
    std::pmr::vector<uint32_t> execution_position(
        graph.passes.size(), RenderGraph::InvalidIndex, &scratch_memory
    );
    for (uint32_t position = 0; position < graph.compiled_plan.execution_order.size(); ++position) {
        execution_position[graph.compiled_plan.execution_order[position].pass_id] = position;
    }

    for (const auto& access : graph.compiled_plan.accesses) {
        auto&          resource = graph.resources[access.resource.resource_id];
        const uint32_t position = execution_position[access.pass.pass_id];
        resource.first_use      = std::min(resource.first_use, position);
        resource.last_use       = resource.last_use == RenderGraph::InvalidIndex ?
                                      position :
                                      std::max(resource.last_use, position);
    }

    for (uint32_t pass_index = 0; pass_index < graph.passes.size(); ++pass_index) {
        const uint32_t position = execution_position[pass_index];
        for (const auto reference : graph.passes[pass_index].references) {
            auto& resource = graph.resources[reference.resource_id];
            resource.first_use = std::min(resource.first_use, position);
            resource.last_use  = resource.last_use == RenderGraph::InvalidIndex ?
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
            .transient_slot = RenderGraph::InvalidIndex,
            .imported      = resource.imported,
            .exported      = resource.exported
        });
    }
}

void RenderGraphCompiler::BuildTransientAliasPlan() {
    using CompiledAccess = RenderGraph::CompiledAccess;

    struct Candidate {
        uint32_t              resource_index = RenderGraph::InvalidIndex;
        const CompiledAccess* first_access   = nullptr;
        const CompiledAccess* last_access    = nullptr;
        bool                  reusable       = false;
    };

    struct AliasSlot {
        uint32_t                  id              = RenderGraph::InvalidIndex;
        uint32_t                  tail_resource   = RenderGraph::InvalidIndex;
        uint32_t                  available_after = RenderGraph::InvalidIndex;
    };

    const auto is_whole_range =
        [](const RenderGraph::ResourceDeclaration& resource,
           const RenderGraph::ResourceRange&       range) {
            if (RenderGraph::GetRangeKind(range) != resource.kind) {
                return false;
            }
            if (resource.kind == ResourceKind::Texture) {
                const auto& texture = std::get<RenderGraph::TextureRange>(range);
                return texture.aspects == resource.texture_desc.aspects && texture.mip_first == 0 &&
                       texture.mip_count == resource.texture_desc.mip_count && texture.layer_first == 0 &&
                       texture.layer_count == resource.texture_desc.layer_count;
            }
            if (resource.kind == ResourceKind::Buffer) {
                const auto& buffer = std::get<RenderGraph::BufferRange>(range);
                return buffer.offset == 0 && buffer.size == resource.buffer_desc.byte_size;
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

    std::pmr::vector<uint32_t> execution_position(
        graph.passes.size(),
        RenderGraph::InvalidIndex,
        &scratch_memory
    );
    for (uint32_t position = 0; position < graph.compiled_plan.execution_order.size();
         ++position) {
        execution_position[graph.compiled_plan.execution_order[position].pass_id] = position;
    }

    std::pmr::vector<Candidate> candidates{&scratch_memory};
    candidates.reserve(graph.resources.size());
    std::pmr::vector<const CompiledAccess*> accesses{&scratch_memory};
    for (uint32_t resource_index = 0; resource_index < graph.resources.size();
         ++resource_index) {
        auto&       compiled_resource = graph.compiled_plan.resources[resource_index];
        const auto& resource          = graph.resources[resource_index];
        const bool  allocation_backed =
            !resource.imported &&
            (resource.transient_texture_desc.has_value() ||
             resource.transient_buffer_desc.has_value());
        if (!allocation_backed ||
            compiled_resource.first_use == RenderGraph::InvalidIndex) {
            continue;
        }

        Candidate candidate{.resource_index = resource_index};
        accesses.clear();
        for (const auto& access : graph.compiled_plan.accesses) {
            if (access.resource.resource_id == resource_index) {
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
                            return reference.resource_id == resource_index;
                        }
                    );
                }
            );

        uint32_t first_access_count = 0;
        uint32_t last_access_count  = 0;
        bool     accesses_are_safe  = !accesses.empty();
        for (const auto* access : accesses) {
            const uint32_t position = execution_position[access->pass.pass_id];
            if (position == compiled_resource.first_use) {
                candidate.first_access = access;
                ++first_access_count;
            }
            if (position == compiled_resource.last_use) {
                candidate.last_access = access;
                ++last_access_count;
            }
            accesses_are_safe &=
                position != RenderGraph::InvalidIndex &&
                access->domain.queue == RenderGraph::QueueRole::Graphics &&
                !RenderGraph::IsUnspecified(access->state) && !RenderGraph::IsUndefined(access->state) &&
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
    std::pmr::vector<AliasSlot> reusable_slots{&scratch_memory};
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
                    return execution_position[lhs.pass.pass_id] <
                           execution_position[rhs.pass.pass_id];
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
            RenderGraph::InvalidIndex;
        if (selected_slot != nullptr && predecessor_cell != nullptr &&
            primary_source != nullptr &&
            execution_position[primary_source->pass.pass_id] <
                compiled_resource.first_use) {
            for (uint32_t barrier_index = 0;
                 barrier_index < graph.compiled_plan.barriers.size();
                 ++barrier_index) {
                auto& barrier = graph.compiled_plan.barriers[barrier_index];
                if (barrier.resource.resource_id == candidate.resource_index &&
                    barrier.dst_pass == candidate.first_access->pass &&
                    barrier.range == candidate.first_access->range &&
                    !barrier.src_pass.IsValid() && barrier.sources.empty() &&
                    RenderGraph::IsUndefined(barrier.before_state) &&
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
                execution_position[source.pass.pass_id] <
                compiled_resource.first_use
            );
            AddEdge(
                source.pass.pass_id,
                candidate.first_access->pass.pass_id,
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
    if (!graph.compiled_plan.alias_boundaries.empty()) {
        SortEdges();
    }
}

bool RenderGraphCompiler::ValidateTransientAliasPlan() {
    const auto& plan = graph.compiled_plan;
    if (plan.resources.size() != graph.resources.size()) {
        return Fail("transient alias plan has an incomplete resource table");
    }

    std::pmr::vector<uint32_t> execution_position(
        graph.passes.size(),
        RenderGraph::InvalidIndex,
        &scratch_memory
    );
    std::pmr::vector<uint32_t> topological_position(
        graph.passes.size(),
        RenderGraph::InvalidIndex,
        &scratch_memory
    );
    for (uint32_t position = 0; position < plan.execution_order.size(); ++position) {
        execution_position[plan.execution_order[position].pass_id] = position;
    }
    for (uint32_t position = 0; position < plan.topological_order.size(); ++position) {
        topological_position[plan.topological_order[position].pass_id] = position;
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
            topological_position[edge.src.pass_id] ==
                RenderGraph::InvalidIndex ||
            topological_position[edge.dst.pass_id] ==
                RenderGraph::InvalidIndex ||
            topological_position[edge.src.pass_id] >=
                topological_position[edge.dst.pass_id]) {
            return Fail(
                "transient alias planning invalidated the compiled topological order"
            );
        }
    }

    std::pmr::vector<uint32_t> alias_barrier_owners(plan.barriers.size(), 0, &scratch_memory);
    for (const auto& alias : plan.alias_boundaries) {
        if (!graph.IsValidResource(alias.predecessor_resource) ||
            !graph.IsValidResource(alias.successor_resource) ||
            !graph.IsValidPass(alias.primary_src_pass) ||
            !graph.IsValidPass(alias.dst_pass) ||
            alias.barrier_index >= plan.barriers.size() ||
            alias.source_frontier.empty()) {
            return Fail("transient alias boundary contains an invalid handle or index");
        }

        const uint32_t predecessor_index = alias.predecessor_resource.resource_id;
        const uint32_t successor_index   = alias.successor_resource.resource_id;
        const auto& predecessor = plan.resources[predecessor_index];
        const auto& successor   = plan.resources[successor_index];
        const auto& predecessor_declaration = graph.resources[predecessor_index];
        const auto& successor_declaration   = graph.resources[successor_index];
        if (predecessor.transient_slot == RenderGraph::InvalidIndex ||
            predecessor.transient_slot != alias.transient_slot ||
            successor.transient_slot != alias.transient_slot ||
            predecessor.first_use == RenderGraph::InvalidIndex ||
            predecessor.last_use == RenderGraph::InvalidIndex ||
            successor.first_use == RenderGraph::InvalidIndex ||
            successor.last_use == RenderGraph::InvalidIndex ||
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
                execution_position[source.pass_id] ==
                    RenderGraph::InvalidIndex ||
                execution_position[alias.dst_pass.pass_id] ==
                    RenderGraph::InvalidIndex ||
                execution_position[source.pass_id] >=
                    execution_position[alias.dst_pass.pass_id] ||
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
        if (resource.transient_slot != RenderGraph::InvalidIndex) {
            max_slot = std::max(max_slot, resource.transient_slot);
            has_slot = true;
        }
    }
    std::pmr::vector<uint32_t> occupants{&scratch_memory};
    for (uint32_t slot = 0; has_slot && slot <= max_slot; ++slot) {
        occupants.clear();
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
            if (resource.first_use == RenderGraph::InvalidIndex ||
                resource.last_use == RenderGraph::InvalidIndex ||
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
                           alias.predecessor_resource.resource_id ==
                               predecessor_index &&
                           alias.successor_resource.resource_id == resource_index;
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
    if (lhs.index() != rhs.index()) {
        return false;
    }
    if (const auto* left = std::get_if<RenderGraph::TextureRange>(&lhs)) {
        const auto& right = std::get<RenderGraph::TextureRange>(rhs);
        if ((AspectMask(left->aspects) & AspectMask(right.aspects)) == 0) {
            return false;
        }
        const uint32_t lhs_mip_end   = left->mip_first + left->mip_count;
        const uint32_t rhs_mip_end   = right.mip_first + right.mip_count;
        const uint32_t lhs_layer_end = left->layer_first + left->layer_count;
        const uint32_t rhs_layer_end = right.layer_first + right.layer_count;
        return left->mip_first < rhs_mip_end && right.mip_first < lhs_mip_end &&
               left->layer_first < rhs_layer_end && right.layer_first < lhs_layer_end;
    }
    if (const auto* left = std::get_if<RenderGraph::BufferRange>(&lhs)) {
        const auto& right = std::get<RenderGraph::BufferRange>(rhs);
        if (left->size == RenderGraph::RemainingBufferRange ||
            right.size == RenderGraph::RemainingBufferRange) {
            return true;
        }
        return left->offset < right.offset + right.size && right.offset < left->offset + left->size;
    }
    return true;
}

bool RenderGraphCompiler::Fail(std::string message) {
    return graph.FailCompile(std::move(message));
}

} // namespace Moer::Render
