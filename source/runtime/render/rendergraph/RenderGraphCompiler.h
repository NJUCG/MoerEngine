#pragma once

#include "rendergraph/RenderGraph.h"

#include <memory_resource>
#include <span>
#include <string>
#include <unordered_map>
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
        RenderGraph::ResourceState  state{};
        bool                        explicit_state = false;
    };

    struct NormalizedStateDeclaration {
        RenderGraph::ResourceRange range{};
        RenderGraph::ResourceState state{};
        RenderGraph::QueueRole     queue = RenderGraph::QueueRole::None;
        RenderGraph::AccessMode    boundary_access = RenderGraph::AccessMode::None;
    };

    struct AtomicCell {
        RenderGraph::ResourceRange range{};
        std::vector<RenderGraph::CompiledBarrierSource> readers{};
        uint32_t                   last_writer = RenderGraph::InvalidIndex;
        RenderGraph::CompiledBarrierSource last_writer_source{};
        uint32_t                   last_access = RenderGraph::InvalidIndex;
        uint32_t                   availability_established_pass = RenderGraph::InvalidIndex;
        uint32_t                   ownership_established_pass = RenderGraph::InvalidIndex;
        uint32_t                   version     = RenderGraph::InvalidVersion;
        RenderGraph::ResourceState state{};
        RenderGraph::ExecutionDomain last_domain{
            RenderGraph::QueueRole::None,
            RenderGraph::GpuWorkType::None
        };
        RenderGraph::AccessMode last_mode = RenderGraph::AccessMode::None;
        bool                       initialized = false;
        bool                       state_known = false;
        bool                       has_explicit_initial_state = false;
    };

    struct CellUsage {
        bool                       read  = false;
        bool                       write = false;
        bool                       explicit_state = false;
        RenderGraph::ResourceState state{};
    };

    bool ValidateQueueTopology();
    bool NormalizeDeclarations();
    bool BuildAtomicCells();
    bool BuildSemanticDependencies();
    bool BuildFinalBarriers();
    void AuditStatePlanCompleteness();
    bool BuildExecutionOrder();
    void BuildQueuePlan();
    std::pmr::vector<uint32_t> BuildDependencyLevels();
    void BuildFrontendRecordUnits(std::span<const uint32_t> dependency_levels);
    void BuildFrontendDispatchGroups();
    void BuildLifetimes();
    void BuildTransientAliasPlan();
    bool ValidateTransientAliasPlan();

    bool NormalizeRange(
        const RenderGraph::ResourceDeclaration& resource,
        const RenderGraph::ResourceRange&       requested,
        RenderGraph::ResourceRange&             normalized
    );
    bool ValidateExecutionDomain(uint32_t pass_index);
    bool ValidateAccessState(
        uint32_t                          pass_index,
        const RenderGraph::ResourceDeclaration& resource,
        const RenderGraph::ResourceRange& range,
        RenderGraph::AccessMode           mode,
        const RenderGraph::ResourceState& state,
        bool                              explicit_state
    );
    bool MergeCellState(
        uint32_t                          pass_index,
        const RenderGraph::ResourceDeclaration& resource,
        CellUsage&                        usage,
        const NormalizedAccess&           access
    );
    void AddBarrier(
        uint32_t                         resource_index,
        const AtomicCell&                cell,
        RenderGraph::PassHandle          dst_pass,
        RenderGraph::ResourceState       next_state,
        RenderGraph::ExecutionDomain     next_domain,
        RenderGraph::AccessMode          next_mode,
        bool                             state_transition,
        bool                             memory_dependency,
        bool                             queue_ownership,
        bool                             import_boundary,
        bool                             export_boundary,
        bool                             source_state_unknown,
        std::vector<RenderGraph::CompiledBarrierSource> sources
    );
    void AddEdge(uint32_t src, uint32_t dst, RenderGraph::CompiledEdgeReason reason);
    void SortEdges();
    bool Fail(std::string message);

    [[nodiscard]] static bool
    RangesOverlap(const RenderGraph::ResourceRange& lhs, const RenderGraph::ResourceRange& rhs);

    RenderGraph& graph;
    // Compiler-only storage dies after Compile; CompiledPlan owns its allocations separately.
    // Declared first so all scratch containers are destroyed before their memory resource.
    std::pmr::monotonic_buffer_resource scratch_memory;
    std::pmr::unordered_map<uint64_t, size_t> edge_indices{&scratch_memory};
    // Rank in the selected execution order, shared by lifetime and alias analysis.
    std::pmr::vector<uint32_t> execution_position{&scratch_memory};
    // polymorphic_allocator propagates this resource to the nested PMR vectors.
    std::pmr::vector<std::pmr::vector<NormalizedAccess>> normalized_accesses{&scratch_memory};
    std::pmr::vector<std::pmr::vector<NormalizedStateDeclaration>> normalized_initial_states{&scratch_memory};
    std::pmr::vector<std::pmr::vector<NormalizedStateDeclaration>> normalized_final_states{&scratch_memory};
    std::pmr::vector<std::pmr::vector<AtomicCell>> resource_cells{&scratch_memory};
    std::pmr::vector<uint32_t> resource_version_counts{&scratch_memory};
    std::pmr::vector<bool> resource_ever_written{std::pmr::polymorphic_allocator<bool>{&scratch_memory}};
};

} // namespace Moer::Render
