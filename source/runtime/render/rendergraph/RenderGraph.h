#pragma once

#include "RenderAPI.h"

#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace Moer::Render {

class RenderGraphCompiler;

/**
 * Serial frontend backed by a typed RDG compiler.
 *
 * The compiler owns typed resource declarations, subresource-aware hazards,
 * logical resource versions, stable dependency analysis and lifetimes. The
 * existing RHI command preprocess remains the only owner of physical tracked
 * state and barriers. Execute deliberately stays synchronous and serial until
 * command recording has an independent ownership contract.
 */
class RENDER_API RenderGraph {
public:
    static constexpr uint32_t RemainingTextureRange = std::numeric_limits<uint32_t>::max();
    static constexpr uint64_t RemainingBufferRange  = std::numeric_limits<uint64_t>::max();
    static constexpr uint32_t InvalidVersion        = std::numeric_limits<uint32_t>::max();

    enum class ResourceKind : uint8_t {
        Texture,
        Buffer,
        Token,
    };

    enum class TextureAspect : uint8_t {
        None    = 0,
        Color   = 1 << 0,
        Depth   = 1 << 1,
        Stencil = 1 << 2,
        All     = (1 << 0) | (1 << 1) | (1 << 2),
    };

    friend constexpr TextureAspect operator|(TextureAspect lhs, TextureAspect rhs) {
        return static_cast<TextureAspect>(static_cast<uint8_t>(lhs) | static_cast<uint8_t>(rhs));
    }

    friend constexpr TextureAspect operator&(TextureAspect lhs, TextureAspect rhs) {
        return static_cast<TextureAspect>(static_cast<uint8_t>(lhs) & static_cast<uint8_t>(rhs));
    }

    struct TextureDesc {
        uint32_t      mip_count   = 1;
        uint32_t      layer_count = 1;
        TextureAspect aspects     = TextureAspect::Color;

        friend bool operator==(const TextureDesc&, const TextureDesc&) = default;
    };

    struct BufferDesc {
        uint64_t byte_size = 0;

        friend bool operator==(const BufferDesc&, const BufferDesc&) = default;
    };

    struct TextureRange {
        TextureAspect aspects     = TextureAspect::All;
        uint32_t      mip_first   = 0;
        uint32_t      mip_count   = RemainingTextureRange;
        uint32_t      layer_first = 0;
        uint32_t      layer_count = RemainingTextureRange;

        [[nodiscard]] static constexpr TextureRange Whole() {
            return {};
        }

        [[nodiscard]] static constexpr TextureRange Mips(uint32_t first, uint32_t count) {
            TextureRange range{};
            range.mip_first = first;
            range.mip_count = count;
            return range;
        }

        [[nodiscard]] static constexpr TextureRange Layers(uint32_t first, uint32_t count) {
            TextureRange range{};
            range.layer_first = first;
            range.layer_count = count;
            return range;
        }

        friend bool operator==(const TextureRange&, const TextureRange&) = default;
    };

    struct BufferRange {
        uint64_t offset = 0;
        uint64_t size   = RemainingBufferRange;

        [[nodiscard]] static constexpr BufferRange Whole() {
            return {};
        }

        friend bool operator==(const BufferRange&, const BufferRange&) = default;
    };

    struct ResourceRange {
        ResourceKind kind = ResourceKind::Token;
        TextureRange texture{};
        BufferRange  buffer{};

        [[nodiscard]] static constexpr ResourceRange Whole(ResourceKind kind) {
            ResourceRange result{};
            result.kind = kind;
            return result;
        }

        [[nodiscard]] static constexpr ResourceRange Texture(TextureRange range = TextureRange::Whole()) {
            ResourceRange result{};
            result.kind    = ResourceKind::Texture;
            result.texture = range;
            return result;
        }

        [[nodiscard]] static constexpr ResourceRange Buffer(BufferRange range = BufferRange::Whole()) {
            ResourceRange result{};
            result.kind   = ResourceKind::Buffer;
            result.buffer = range;
            return result;
        }

        [[nodiscard]] static constexpr ResourceRange Token() {
            ResourceRange result{};
            result.kind = ResourceKind::Token;
            return result;
        }

        friend bool operator==(const ResourceRange&, const ResourceRange&) = default;
    };

    struct ResourceHandle {
        static constexpr uint32_t InvalidIndex = std::numeric_limits<uint32_t>::max();

        uint32_t index    = InvalidIndex;
        uint64_t owner_id = 0;

        [[nodiscard]] bool IsValid() const {
            return index != InvalidIndex && owner_id != 0;
        }

        friend bool operator==(ResourceHandle lhs, ResourceHandle rhs) {
            return lhs.index == rhs.index && lhs.owner_id == rhs.owner_id;
        }
    };

    struct TextureHandle {
        ResourceHandle resource{};

        [[nodiscard]] bool IsValid() const {
            return resource.IsValid();
        }
        [[nodiscard]] ResourceHandle Untyped() const {
            return resource;
        }

        friend bool operator==(TextureHandle lhs, TextureHandle rhs) {
            return lhs.resource == rhs.resource;
        }
    };

    struct BufferHandle {
        ResourceHandle resource{};

        [[nodiscard]] bool IsValid() const {
            return resource.IsValid();
        }
        [[nodiscard]] ResourceHandle Untyped() const {
            return resource;
        }

        friend bool operator==(BufferHandle lhs, BufferHandle rhs) {
            return lhs.resource == rhs.resource;
        }
    };

    struct TokenHandle {
        ResourceHandle resource{};

        [[nodiscard]] bool IsValid() const {
            return resource.IsValid();
        }
        [[nodiscard]] ResourceHandle Untyped() const {
            return resource;
        }

        friend bool operator==(TokenHandle lhs, TokenHandle rhs) {
            return lhs.resource == rhs.resource;
        }
    };

    struct PassHandle {
        static constexpr uint32_t InvalidIndex = std::numeric_limits<uint32_t>::max();

        uint32_t index    = InvalidIndex;
        uint64_t owner_id = 0;

        [[nodiscard]] bool IsValid() const {
            return index != InvalidIndex && owner_id != 0;
        }

        friend bool operator==(PassHandle lhs, PassHandle rhs) {
            return lhs.index == rhs.index && lhs.owner_id == rhs.owner_id;
        }
    };

    enum class AccessMode : uint8_t {
        Read,
        Write,
        ReadWrite,
    };

    enum class EdgeReasonKind : uint8_t {
        Explicit,
        ReadAfterWrite,
        WriteAfterRead,
        WriteAfterWrite,
    };

    struct CompiledAccess {
        PassHandle     pass{};
        ResourceHandle resource{};
        AccessMode     mode = AccessMode::Read;
        ResourceRange  range{};
        uint32_t       input_version  = InvalidVersion;
        uint32_t       output_version = InvalidVersion;
    };

    struct CompiledEdgeReason {
        EdgeReasonKind kind = EdgeReasonKind::Explicit;
        ResourceHandle resource{};
        ResourceRange  range{};
        uint32_t       input_version  = InvalidVersion;
        uint32_t       output_version = InvalidVersion;

        friend bool operator==(const CompiledEdgeReason&, const CompiledEdgeReason&) = default;
    };

    struct CompiledEdge {
        PassHandle                      src{};
        PassHandle                      dst{};
        std::vector<CompiledEdgeReason> reasons{};
    };

    struct CompiledResource {
        ResourceHandle resource{};
        uint32_t       first_use     = PassHandle::InvalidIndex;
        uint32_t       last_use      = PassHandle::InvalidIndex;
        uint32_t       version_count = 0;
        bool           imported      = false;
        bool           exported      = false;
    };

    struct CompiledWave {
        std::vector<PassHandle> passes{};
    };

    struct CompiledPlan {
        /** Stable topological order of semantic dependencies. */
        std::vector<PassHandle> topological_order{};
        /** Current production order. It remains declaration-order and serial. */
        std::vector<PassHandle> execution_order{};
        /** RAW/WAR/WAW and explicit dependencies only, without fake serial edges. */
        std::vector<CompiledEdge> edges{};
        /** Atomic subresource/range accesses with logical input/output versions. */
        std::vector<CompiledAccess>   accesses{};
        std::vector<CompiledResource> resources{};
        /** Dependency-only waves for diagnostics. They are not yet an execution schedule. */
        std::vector<CompiledWave> dependency_waves{};

        void Clear() {
            topological_order.clear();
            execution_order.clear();
            edges.clear();
            accesses.clear();
            resources.clear();
            dependency_waves.clear();
        }
    };

    class RENDER_API PassBuilder {
    public:
        // Legacy whole-resource declarations remain available for compatibility.
        PassBuilder& Read(ResourceHandle resource, std::string_view range = "all");
        PassBuilder& Write(ResourceHandle resource, std::string_view range = "all");
        PassBuilder& ReadWrite(ResourceHandle resource, std::string_view range = "all");

        PassBuilder& Read(TextureHandle resource, TextureRange range = TextureRange::Whole());
        PassBuilder& Write(TextureHandle resource, TextureRange range = TextureRange::Whole());
        PassBuilder& ReadWrite(TextureHandle resource, TextureRange range = TextureRange::Whole());

        PassBuilder& Read(BufferHandle resource, BufferRange range = BufferRange::Whole());
        PassBuilder& Write(BufferHandle resource, BufferRange range = BufferRange::Whole());
        PassBuilder& ReadWrite(BufferHandle resource, BufferRange range = BufferRange::Whole());

        PassBuilder& Read(TokenHandle resource);
        PassBuilder& Write(TokenHandle resource);
        PassBuilder& ReadWrite(TokenHandle resource);

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

    /** Legacy import API. Prefer the typed imports below for new code. */
    ResourceHandle Import(std::string_view name, ResourceKind kind, const void* physical_identity = nullptr);

    TextureHandle ImportTexture(std::string_view name, const void* physical_identity, TextureDesc desc);
    BufferHandle  ImportBuffer(std::string_view name, const void* physical_identity, BufferDesc desc);
    TokenHandle   ImportToken(std::string_view name, const void* physical_identity = nullptr);

    /** Legacy transient API. Prefer the typed declarations below for new code. */
    ResourceHandle CreateTransient(std::string_view name, ResourceKind kind);
    TextureHandle  CreateTransientTexture(std::string_view name, TextureDesc desc);
    BufferHandle   CreateTransientBuffer(std::string_view name, BufferDesc desc);
    TokenHandle    CreateTransientToken(std::string_view name);

    void Export(ResourceHandle resource);
    void Export(TextureHandle resource) {
        Export(resource.Untyped());
    }
    void Export(BufferHandle resource) {
        Export(resource.Untyped());
    }
    void Export(TokenHandle resource) {
        Export(resource.Untyped());
    }

    PassHandle AddPass(std::string_view name, const SetupCallback& setup, ExecuteCallback execute);

    /** Compiles declarations without emitting barriers or recording RHI commands. */
    bool Compile();

    /** Executes callbacks synchronously and serially once in declaration order. */
    bool Execute();

    [[nodiscard]] bool IsCompiled() const {
        return compiled;
    }

    [[nodiscard]] const std::string& GetCompileError() const {
        return compile_error;
    }

    [[nodiscard]] const CompiledPlan& GetCompiledPlan() const {
        return compiled_plan;
    }

    /** Deterministic text dump of passes, typed accesses, edges, versions and lifetimes. */
    [[nodiscard]] std::string Dump() const;

private:
    friend class RenderGraphCompiler;

    struct AccessDeclaration {
        ResourceHandle resource{};
        AccessMode     mode = AccessMode::Read;
        ResourceRange  range{};
        std::string    legacy_range{};
        bool           typed = false;
    };

    struct ResourceDeclaration {
        std::string              name{};
        std::vector<std::string> aliases{};
        ResourceKind             kind              = ResourceKind::Token;
        const void*              physical_identity = nullptr;
        TextureDesc              texture_desc{};
        BufferDesc               buffer_desc{};
        bool                     typed_desc = false;
        bool                     imported   = false;
        bool                     exported   = false;
        uint32_t                 first_use  = PassHandle::InvalidIndex;
        uint32_t                 last_use   = PassHandle::InvalidIndex;
    };

    struct PassDeclaration {
        std::string                    name{};
        std::vector<AccessDeclaration> accesses{};
        std::vector<PassHandle>        explicit_dependencies{};
        ExecuteCallback                execute{};
        bool                           side_effect = false;
    };

    ResourceHandle ImportInternal(
        std::string_view   name,
        ResourceKind       kind,
        const void*        physical_identity,
        const TextureDesc* texture_desc,
        const BufferDesc*  buffer_desc
    );
    ResourceHandle CreateTransientInternal(
        std::string_view   name,
        ResourceKind       kind,
        const TextureDesc* texture_desc,
        const BufferDesc*  buffer_desc
    );
    void AddAccess(
        uint32_t         pass_index,
        ResourceHandle   resource,
        AccessMode       mode,
        ResourceRange    range,
        bool             typed,
        std::string_view legacy_range = {}
    );
    void AddDependency(uint32_t pass_index, PassHandle dependency);
    void MarkSideEffect(uint32_t pass_index);
    bool InvalidateCompile();
    bool FailCompile(std::string message);

    [[nodiscard]] bool IsValidResource(ResourceHandle resource) const;
    [[nodiscard]] bool IsValidPass(PassHandle pass) const;

    std::string                      name{};
    std::vector<ResourceDeclaration> resources{};
    std::vector<PassDeclaration>     passes{};
    CompiledPlan                     compiled_plan{};
    std::vector<std::string>         declaration_errors{};
    std::string                      compile_error{};
    uint64_t                         graph_id = 0;
    bool                             compiled = false;
    bool                             executed = false;
};

} // namespace Moer::Render
