#pragma once

#include "rendergraph/RenderGraph.h"

#include <string>
#include <vector>

namespace Moer::Render {

/** Internal compiler for RenderGraph declarations. */
class RenderGraphCompiler {
public:
    explicit RenderGraphCompiler(RenderGraph& graph) : graph(graph) {}

    bool Compile();

private:
    struct NormalizedAccess {
        RenderGraph::ResourceHandle resource{};
        RenderGraph::AccessMode     mode = RenderGraph::AccessMode::Read;
        RenderGraph::ResourceRange  range{};
    };

    struct AtomicCell {
        RenderGraph::ResourceRange range{};
        std::vector<uint32_t>      readers{};
        uint32_t                   last_writer = RenderGraph::PassHandle::InvalidIndex;
        uint32_t                   version     = RenderGraph::InvalidVersion;
        bool                       initialized = false;
    };

    struct CellUsage {
        bool read  = false;
        bool write = false;
    };

    bool NormalizeDeclarations();
    bool BuildAtomicCells();
    bool BuildSemanticDependencies();
    bool BuildTopologicalOrder();
    bool BuildExecutionOrder();
    void BuildDependencyWaves();
    void BuildLifetimes();

    bool NormalizeRange(
        const RenderGraph::ResourceDeclaration& resource,
        const RenderGraph::AccessDeclaration&   access,
        RenderGraph::ResourceRange&             normalized
    );
    void AddEdge(uint32_t src, uint32_t dst, RenderGraph::CompiledEdgeReason reason);
    bool Fail(std::string message);

    [[nodiscard]] static bool
    RangesOverlap(const RenderGraph::ResourceRange& lhs, const RenderGraph::ResourceRange& rhs);

    RenderGraph&                               graph;
    std::vector<std::vector<NormalizedAccess>> normalized_accesses{};
    std::vector<std::vector<AtomicCell>>       resource_cells{};
    std::vector<uint32_t>                      resource_version_counts{};
    std::vector<bool>                          resource_ever_written{};
};

} // namespace Moer::Render
