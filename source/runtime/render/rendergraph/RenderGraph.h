#pragma once

#include "RenderAPI.h"

#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace Moer::Render {

/**
 * Phase 8 serial RenderGraph.
 *
 * The graph owns declaration, dependency validation, stable scheduling and
 * logical transient lifetimes only. It deliberately does not emit resource
 * barriers or mutate RHI tracked state: command reorder / backend preprocess
 * remains the single owner of real barriers.
 */
class RENDER_API RenderGraph {
public:
    enum class ResourceKind : uint8_t {
        Texture,
        Buffer,
        Token,
    };

    struct ResourceHandle {
        static constexpr uint32_t InvalidIndex = std::numeric_limits<uint32_t>::max();

        uint32_t index = InvalidIndex;
        uint64_t owner_id = 0;

        [[nodiscard]] bool IsValid() const {
            return index != InvalidIndex && owner_id != 0;
        }

        friend bool operator==(ResourceHandle lhs, ResourceHandle rhs) {
            return lhs.index == rhs.index && lhs.owner_id == rhs.owner_id;
        }
    };

    struct PassHandle {
        static constexpr uint32_t InvalidIndex = std::numeric_limits<uint32_t>::max();

        uint32_t index = InvalidIndex;
        uint64_t owner_id = 0;

        [[nodiscard]] bool IsValid() const {
            return index != InvalidIndex && owner_id != 0;
        }

        friend bool operator==(PassHandle lhs, PassHandle rhs) {
            return lhs.index == rhs.index && lhs.owner_id == rhs.owner_id;
        }
    };

    class RENDER_API PassBuilder {
    public:
        PassBuilder& Read(ResourceHandle resource, std::string_view range = "all");
        PassBuilder& Write(ResourceHandle resource, std::string_view range = "all");
        PassBuilder& ReadWrite(ResourceHandle resource, std::string_view range = "all");
        PassBuilder& DependsOn(PassHandle dependency);
        PassBuilder& SideEffect();

    private:
        PassBuilder(RenderGraph& graph, uint32_t pass_index) : graph(graph), pass_index(pass_index) {}

        RenderGraph& graph;
        uint32_t     pass_index;

        friend class RenderGraph;
    };

    using SetupCallback   = std::function<void(PassBuilder&)>;
    using ExecuteCallback = std::function<void()>;

    explicit RenderGraph(std::string_view name = "RenderGraph");
    ~RenderGraph();

    RenderGraph(const RenderGraph&)            = delete;
    RenderGraph& operator=(const RenderGraph&) = delete;
    RenderGraph(RenderGraph&&)                 = delete;
    RenderGraph& operator=(RenderGraph&&)      = delete;

    /**
     * Imports a resource owned outside the graph. A non-null physical identity
     * is de-duplicated, so multiple view/sampler names can alias one resource.
     */
    ResourceHandle Import(
        std::string_view name,
        ResourceKind    kind,
        const void*     physical_identity = nullptr
    );

    /**
     * Declares a logical transient lifetime. Phase 8 records first/last use
     * only; physical allocation, release and alias reuse intentionally remain
     * with the existing typed resource / GPU-completion ownership system.
     */
    ResourceHandle CreateTransient(
        std::string_view name,
        ResourceKind    kind
    );

    void Export(ResourceHandle resource);

    PassHandle AddPass(
        std::string_view name,
        const SetupCallback& setup,
        ExecuteCallback      execute
    );

    /** Returns false and stores a deterministic diagnostic when the graph is invalid. */
    bool Compile();

    /** Executes callbacks serially once in the compiled stable topological order. */
    bool Execute();

    [[nodiscard]] bool IsCompiled() const {
        return compiled;
    }

    [[nodiscard]] const std::string& GetCompileError() const {
        return compile_error;
    }

    /** Deterministic text dump of passes, accesses, edges and lifetimes. */
    [[nodiscard]] std::string Dump() const;

private:
    enum class AccessMode : uint8_t {
        Read,
        Write,
        ReadWrite,
    };

    struct AccessDeclaration {
        ResourceHandle resource;
        AccessMode     mode = AccessMode::Read;
        std::string    range;
    };

    struct ResourceDeclaration {
        std::string                name;
        std::vector<std::string>   aliases;
        ResourceKind               kind = ResourceKind::Token;
        const void*                physical_identity = nullptr;
        bool                       imported = false;
        bool                       exported = false;
        uint32_t                   first_use = PassHandle::InvalidIndex;
        uint32_t                   last_use = PassHandle::InvalidIndex;
    };

    struct PassDeclaration {
        std::string                    name;
        std::vector<AccessDeclaration> accesses;
        std::vector<PassHandle>        explicit_dependencies;
        ExecuteCallback                execute;
        bool                           side_effect = false;
    };

    struct CompiledEdge {
        uint32_t                 src = PassHandle::InvalidIndex;
        uint32_t                 dst = PassHandle::InvalidIndex;
        std::vector<std::string> reasons;
    };

    void AddAccess(uint32_t pass_index, ResourceHandle resource, AccessMode mode, std::string_view range);
    void AddDependency(uint32_t pass_index, PassHandle dependency);
    void MarkSideEffect(uint32_t pass_index);
    bool InvalidateCompile();
    bool FailCompile(std::string message);

    [[nodiscard]] bool IsValidResource(ResourceHandle resource) const;
    [[nodiscard]] bool IsValidPass(PassHandle pass) const;

    std::string                      name;
    std::vector<ResourceDeclaration> resources;
    std::vector<PassDeclaration>     passes;
    std::vector<uint32_t>            execution_order;
    std::vector<CompiledEdge>        compiled_edges;
    std::vector<std::string>         declaration_errors;
    std::string                      compile_error;
    uint64_t                         graph_id = 0;
    bool                             compiled = false;
    bool                             executed = false;
};

} // namespace Moer::Render
