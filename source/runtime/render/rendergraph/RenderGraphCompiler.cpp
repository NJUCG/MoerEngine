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
    resource_cells.assign(graph.resources.size(), {});
    resource_version_counts.assign(graph.resources.size(), 0);
    resource_ever_written.assign(graph.resources.size(), false);

    if (!NormalizeDeclarations() || !BuildAtomicCells() || !BuildSemanticDependencies() ||
        !BuildTopologicalOrder() || !BuildExecutionOrder()) {
        return false;
    }

    BuildDependencyWaves();
    BuildLifetimes();
    graph.compiled = true;
    return true;
}

bool RenderGraphCompiler::NormalizeDeclarations() {
    for (uint32_t pass_index = 0; pass_index < graph.passes.size(); ++pass_index) {
        const auto& pass = graph.passes[pass_index];
        for (const auto& access : pass.accesses) {
            if (!graph.IsValidResource(access.resource)) {
                return Fail("pass '" + pass.name + "' references an invalid resource");
            }
            const auto&   resource = graph.resources[access.resource.index];
            ResourceRange normalized{};
            if (!NormalizeRange(resource, access, normalized)) {
                return false;
            }
            normalized_accesses[pass_index].push_back(
                NormalizedAccess{access.resource, access.mode, normalized}
            );
        }
    }
    return true;
}

bool RenderGraphCompiler::NormalizeRange(
    const RenderGraph::ResourceDeclaration& resource,
    const RenderGraph::AccessDeclaration&   access,
    ResourceRange&                          normalized
) {
    if (access.range.kind != resource.kind) {
        return Fail("resource range kind does not match resource '" + resource.name + "'");
    }

    normalized = access.range;
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
            for (const auto& accesses : normalized_accesses) {
                for (const auto& access : accesses) {
                    if (access.resource.index != resource_index) {
                        continue;
                    }
                    mip_boundaries.push_back(access.range.texture.mip_first);
                    mip_boundaries.push_back(access.range.texture.mip_first + access.range.texture.mip_count);
                    layer_boundaries.push_back(access.range.texture.layer_first);
                    layer_boundaries.push_back(
                        access.range.texture.layer_first + access.range.texture.layer_count
                    );
                }
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
                        cells.push_back(std::move(cell));
                    }
                }
            }
        } else if (resource.kind == ResourceKind::Buffer && resource.buffer_desc.byte_size != 0) {
            std::vector<uint64_t> boundaries{0, resource.buffer_desc.byte_size};
            for (const auto& accesses : normalized_accesses) {
                for (const auto& access : accesses) {
                    if (access.resource.index != resource_index) {
                        continue;
                    }
                    boundaries.push_back(access.range.buffer.offset);
                    boundaries.push_back(access.range.buffer.offset + access.range.buffer.size);
                }
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
                cells.push_back(std::move(cell));
            }
        } else {
            AtomicCell cell{};
            cell.range       = ResourceRange::Whole(resource.kind);
            cell.initialized = resource.imported;
            cell.version     = resource.imported ? 0 : RenderGraph::InvalidVersion;
            cells.push_back(std::move(cell));
        }

        if (cells.empty()) {
            return Fail("resource produced no compiler intervals: " + resource.name);
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
                usage.read |= access.mode == RenderGraph::AccessMode::Read ||
                              access.mode == RenderGraph::AccessMode::ReadWrite;
                usage.write |= access.mode == RenderGraph::AccessMode::Write ||
                               access.mode == RenderGraph::AccessMode::ReadWrite;
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
                    for (const uint32_t reader : cell.readers) {
                        AddEdge(
                            reader,
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
                    cell.version                          = output_version;
                    cell.initialized                      = true;
                    resource_ever_written[resource_index] = true;
                } else if (std::find(cell.readers.begin(), cell.readers.end(), pass_index) ==
                           cell.readers.end()) {
                    cell.readers.push_back(pass_index);
                }

                graph.compiled_plan.accesses.push_back(RenderGraph::CompiledAccess{
                    .pass           = RenderGraph::PassHandle{pass_index, graph.graph_id},
                    .resource       = resource_handle,
                    .mode           = ToAccessMode(usage.read, usage.write),
                    .range          = cell.range,
                    .input_version  = input_version,
                    .output_version = usage.write ? output_version : input_version
                });
            }
        }
    }

    for (uint32_t resource_index = 0; resource_index < graph.resources.size(); ++resource_index) {
        const auto& resource = graph.resources[resource_index];
        if (!resource.imported && !resource_ever_written[resource_index]) {
            return Fail("transient resource has no producer: " + resource.name);
        }
        if (!resource.imported && resource.exported) {
            const bool fully_initialized = std::all_of(
                resource_cells[resource_index].begin(),
                resource_cells[resource_index].end(),
                [](const AtomicCell& cell) {
                    return cell.initialized;
                }
            );
            if (!fully_initialized) {
                return Fail("exported transient resource has uninitialized subresources: " + resource.name);
            }
        }
    }
    return true;
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

    graph.compiled_plan.resources.reserve(graph.resources.size());
    for (uint32_t resource_index = 0; resource_index < graph.resources.size(); ++resource_index) {
        const auto& resource = graph.resources[resource_index];
        graph.compiled_plan.resources.push_back(RenderGraph::CompiledResource{
            .resource      = RenderGraph::ResourceHandle{resource_index, graph.graph_id},
            .first_use     = resource.first_use,
            .last_use      = resource.last_use,
            .version_count = resource_version_counts[resource_index],
            .imported      = resource.imported,
            .exported      = resource.exported
        });
    }
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
