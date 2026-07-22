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
 * RDG does not own physical tracked state or emit barriers in this stage; the
 * existing RHI/Vulkan command path remains responsible. Execute deliberately
 * stays synchronous and serial until command recording has an independent
 * ownership contract.
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
        enum class SharingMode : uint8_t {
            Exclusive,
            Concurrent,
        } sharing_mode = SharingMode::Exclusive;

        friend bool operator==(const TextureDesc&, const TextureDesc&) = default;
    };

    struct BufferDesc {
        uint64_t byte_size = 0;
        TextureDesc::SharingMode sharing_mode = TextureDesc::SharingMode::Exclusive;

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
        Unknown,
        None,
        Read,
        Write,
        ReadWrite,
    };

    /** Logical queue role. It is deliberately separate from a native queue and queue family. */
    enum class QueueRole : uint8_t {
        None,
        Graphics,
        Compute,
        Copy,
    };

    /** Pipeline domain used to derive source/destination stage scopes during future RHI lowering. */
    enum class PipelineType : uint8_t {
        None,
        Graphics,
        Compute,
        RayTracing,
        Copy,
    };

    struct QueueBinding {
        QueueRole role            = QueueRole::None;
        uint32_t  native_queue_id = 0;
        uint32_t  family_id       = 0;

        friend bool operator==(const QueueBinding&, const QueueBinding&) = default;
    };

    /**
     * Maps logical roles to actual queues. Tests may provide a fake topology; production currently
     * uses SingleQueue() until the RHI submission path consumes the compiled plan.
     */
    struct QueueTopology {
        QueueBinding graphics{QueueRole::Graphics, 0, 0};
        QueueBinding compute{QueueRole::Compute, 0, 0};
        QueueBinding copy{QueueRole::Copy, 0, 0};

        [[nodiscard]] static constexpr QueueTopology SingleQueue() {
            return {};
        }

        [[nodiscard]] static constexpr QueueTopology DedicatedQueues() {
            return QueueTopology{
                .graphics = QueueBinding{QueueRole::Graphics, 0, 0},
                .compute  = QueueBinding{QueueRole::Compute, 1, 1},
                .copy     = QueueBinding{QueueRole::Copy, 2, 2},
            };
        }

        [[nodiscard]] constexpr QueueBinding Resolve(QueueRole role) const {
            switch (role) {
                case QueueRole::Graphics:
                    return graphics;
                case QueueRole::Compute:
                    return compute;
                case QueueRole::Copy:
                    return copy;
                case QueueRole::None:
                    return {};
            }
            return {};
        }

        friend bool operator==(const QueueTopology&, const QueueTopology&) = default;
    };

    struct ExecutionDomain {
        QueueRole    queue    = QueueRole::Graphics;
        PipelineType pipeline = PipelineType::Graphics;

        friend bool operator==(const ExecutionDomain&, const ExecutionDomain&) = default;
    };

    /** RDG-neutral states; these do not depend on the legacy RHI enum ordinals. */
    enum class TextureState : uint8_t {
        Automatic,
        Undefined,
        TransferSource,
        TransferDestination,
        ShaderResource,
        Sampled,
        RenderTarget,
        DepthStencilRead,
        DepthStencilWrite,
        UnorderedAccess,
        Present,
    };

    enum class BufferState : uint8_t {
        Automatic,
        Undefined,
        TransferSource,
        TransferDestination,
        VertexBuffer,
        IndexBuffer,
        IndirectArgument,
        ShaderResource,
        UnorderedAccess,
        AccelerationStructureBuildInput,
        AccelerationStructureRead,
        AccelerationStructureWrite,
    };

    struct ResourceState {
        ResourceKind kind = ResourceKind::Token;
        TextureState texture = TextureState::Automatic;
        BufferState  buffer  = BufferState::Automatic;

        [[nodiscard]] static constexpr ResourceState Texture(TextureState state) {
            ResourceState result{};
            result.kind    = ResourceKind::Texture;
            result.texture = state;
            return result;
        }

        [[nodiscard]] static constexpr ResourceState Buffer(BufferState state) {
            ResourceState result{};
            result.kind   = ResourceKind::Buffer;
            result.buffer = state;
            return result;
        }

        [[nodiscard]] static constexpr ResourceState Token() {
            return {};
        }

        [[nodiscard]] constexpr bool IsAutomatic() const {
            return kind == ResourceKind::Texture ? texture == TextureState::Automatic :
                   kind == ResourceKind::Buffer  ? buffer == BufferState::Automatic :
                                                   true;
        }

        [[nodiscard]] constexpr bool IsUndefined() const {
            return kind == ResourceKind::Texture ? texture == TextureState::Undefined :
                   kind == ResourceKind::Buffer  ? buffer == BufferState::Undefined :
                                                   false;
        }

        friend bool operator==(const ResourceState&, const ResourceState&) = default;
    };

    enum class EdgeReasonKind : uint8_t {
        Explicit,
        ReadAfterWrite,
        WriteAfterRead,
        WriteAfterWrite,
        /** State/data availability established on another native queue. */
        StateTransition,
        QueueOwnership,
    };

    struct CompiledAccess {
        PassHandle     pass{};
        ResourceHandle resource{};
        AccessMode     mode = AccessMode::Read;
        ResourceRange  range{};
        ResourceState  state{};
        ExecutionDomain domain{};
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

    /**
     * One canonical synchronization decision for one atomic resource cell. The future backend may
     * lower a queue_ownership record into paired release/acquire barriers; RDG itself does not emit
     * physical barriers in this stage.
     */
    struct CompiledBarrierSource {
        PassHandle      pass{};
        ResourceState   state{};
        AccessMode      access = AccessMode::None;
        ExecutionDomain domain{QueueRole::None, PipelineType::None};

        friend bool operator==(const CompiledBarrierSource&, const CompiledBarrierSource&) = default;
    };

    struct CompiledBarrier {
        ResourceHandle  resource{};
        ResourceRange   range{};
        /** Latest actual predecessor in sources; invalid for an external/undefined boundary. */
        PassHandle      src_pass{};
        PassHandle      dst_pass{};
        /** Current tracked state at the destination boundary, independent of source fan-in. */
        ResourceState   before_state{};
        ResourceState   after_state{};
        /** Representative source scope; sources is authoritative when it is non-empty. */
        AccessMode      before_access = AccessMode::None;
        AccessMode      after_access  = AccessMode::None;
        ExecutionDomain src_domain{QueueRole::None, PipelineType::None};
        ExecutionDomain dst_domain{QueueRole::None, PipelineType::None};
        bool             state_transition = false;
        bool             memory_dependency = false;
        bool             execution_dependency = false;
        bool             queue_dependency = false;
        bool             queue_ownership = false;
        bool             discard_previous_contents = false;
        bool             import_boundary = false;
        bool             export_boundary = false;
        bool             source_state_unknown = false;
        /** Authoritative synchronization frontier whose scopes must be represented by lowering. */
        std::vector<CompiledBarrierSource> sources{};
    };

    struct CompiledQueueSync {
        uint32_t     id = 0;
        PassHandle   signal_pass{};
        PassHandle   wait_pass{};
        QueueBinding signal_queue{};
        QueueBinding wait_queue{};
        uint32_t     signal_batch = PassHandle::InvalidIndex;
        uint32_t     wait_batch   = PassHandle::InvalidIndex;
        bool         gpu_wait_required = false;
        std::vector<uint32_t> dependency_edges{};
        std::vector<uint32_t> barriers{};
    };

    struct CompiledPassBarriers {
        PassHandle            pass{};
        std::vector<uint32_t> before{};
    };

    struct CompiledQueueBatch {
        uint32_t                id = 0;
        QueueBinding            queue{};
        std::vector<PassHandle> passes{};
        /**
         * Correlated split acquire/release placement hints. An index in pre/post never means that the
         * full CompiledBarrier should be emitted again; pass_barriers and prologue/epilogue remain the
         * canonical boundaries. Export transitions are deliberately epilogue-only.
         */
        std::vector<uint32_t>   pre_barriers{};
        std::vector<uint32_t>   post_barriers{};
        std::vector<uint32_t>   wait_syncs{};
        std::vector<uint32_t>   signal_syncs{};
    };

    /**
     * Immutable compiler diagnostics for the current serial executor. This is shadow metadata,
     * not an executable submission plan: queue_syncs only describe internal pass/batch edges,
     * while external import/export/present synchronization endpoints are intentionally absent.
     */
    struct CompiledPlan {
        /** Stable topological order of semantic dependencies. */
        std::vector<PassHandle> topological_order{};
        /** Current production order. It remains declaration-order and serial. */
        std::vector<PassHandle> execution_order{};
        /** Minimal hazard, explicit, state, and ownership frontier without fake serial edges. */
        std::vector<CompiledEdge> edges{};
        /** Atomic subresource/range accesses with logical input/output versions. */
        std::vector<CompiledAccess>   accesses{};
        std::vector<CompiledResource> resources{};
        /** Dependency-only waves for diagnostics. They are not yet an execution schedule. */
        std::vector<CompiledWave> dependency_waves{};
        /** Canonical state/memory/ownership decisions, prior to backend-specific lowering. */
        std::vector<CompiledBarrier> barriers{};
        /** Prospective multi-queue batches and their GPU wait/signal relationships. */
        std::vector<CompiledQueueBatch> queue_batches{};
        std::vector<CompiledQueueSync>  queue_syncs{};
        std::vector<CompiledPassBarriers> pass_barriers{};
        /** Boundary requirements only; active lowering must bind external synchronization first. */
        std::vector<uint32_t> prologue_barriers{};
        std::vector<uint32_t> epilogue_barriers{};
        /**
         * False when a participating physical cell has no logical state. True only means the
         * shadow state model is complete; it does not mean this plan can be lowered or submitted.
         */
        bool state_plan_complete = true;

        void Clear() {
            topological_order.clear();
            execution_order.clear();
            edges.clear();
            accesses.clear();
            resources.clear();
            dependency_waves.clear();
            barriers.clear();
            queue_batches.clear();
            queue_syncs.clear();
            pass_barriers.clear();
            prologue_barriers.clear();
            epilogue_barriers.clear();
            state_plan_complete = true;
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
        PassBuilder& Read(
            TextureHandle resource,
            TextureState  state,
            TextureRange  range = TextureRange::Whole()
        );
        PassBuilder& Write(
            TextureHandle resource,
            TextureState  state,
            TextureRange  range = TextureRange::Whole()
        );
        PassBuilder& ReadWrite(
            TextureHandle resource,
            TextureState  state,
            TextureRange  range = TextureRange::Whole()
        );

        PassBuilder& Read(BufferHandle resource, BufferRange range = BufferRange::Whole());
        PassBuilder& Write(BufferHandle resource, BufferRange range = BufferRange::Whole());
        PassBuilder& ReadWrite(BufferHandle resource, BufferRange range = BufferRange::Whole());
        PassBuilder& Read(
            BufferHandle resource,
            BufferState  state,
            BufferRange  range = BufferRange::Whole()
        );
        PassBuilder& Write(
            BufferHandle resource,
            BufferState  state,
            BufferRange  range = BufferRange::Whole()
        );
        PassBuilder& ReadWrite(
            BufferHandle resource,
            BufferState  state,
            BufferRange  range = BufferRange::Whole()
        );

        PassBuilder& Read(TokenHandle resource);
        PassBuilder& Write(TokenHandle resource);
        PassBuilder& ReadWrite(TokenHandle resource);

        PassBuilder& DependsOn(PassHandle dependency);
        PassBuilder& SideEffect();
        PassBuilder& ExecuteOn(QueueRole queue, PipelineType pipeline);

    private:
        PassBuilder(RenderGraph& graph, uint32_t pass_index) : graph(graph), pass_index(pass_index) {}

        RenderGraph& graph;
        uint32_t     pass_index;

        friend class RenderGraph;
    };

    using SetupCallback   = std::function<void(PassBuilder&)>;
    using ExecuteCallback = std::function<void()>;

    explicit RenderGraph(std::string_view name = "RenderGraph");
    RenderGraph(std::string_view name, QueueTopology topology);
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

    void SetInitialState(
        TextureHandle resource,
        TextureState  state,
        QueueRole     owner_queue,
        AccessMode    last_access = AccessMode::Unknown,
        TextureRange  range       = TextureRange::Whole()
    );
    void SetInitialState(
        BufferHandle resource,
        BufferState  state,
        QueueRole    owner_queue,
        AccessMode   last_access = AccessMode::Unknown,
        BufferRange  range       = BufferRange::Whole()
    );
    void Export(
        TextureHandle resource,
        TextureState  final_state,
        QueueRole     owner_queue,
        AccessMode    next_access = AccessMode::Read,
        TextureRange  range       = TextureRange::Whole()
    );
    void Export(
        BufferHandle resource,
        BufferState  final_state,
        QueueRole    owner_queue,
        AccessMode   next_access = AccessMode::Read,
        BufferRange  range       = BufferRange::Whole()
    );

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

    /** Returns non-executable shadow compiler metadata; see CompiledPlan's contract. */
    [[nodiscard]] const CompiledPlan& GetCompiledPlan() const {
        return compiled_plan;
    }

    [[nodiscard]] const QueueTopology& GetQueueTopology() const {
        return queue_topology;
    }

    /** Deterministic text dump of passes, typed accesses, edges, versions and lifetimes. */
    [[nodiscard]] std::string Dump() const;

private:
    friend class RenderGraphCompiler;

    struct AccessDeclaration {
        ResourceHandle resource{};
        AccessMode     mode = AccessMode::Read;
        ResourceRange  range{};
        ResourceState  state{};
        std::string    legacy_range{};
        bool           typed = false;
        bool           explicit_state = false;
    };

    struct StateDeclaration {
        ResourceRange range{};
        ResourceState state{};
        QueueRole     queue       = QueueRole::None;
        AccessMode    boundary_access = AccessMode::None;
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
        bool                     whole_resource_exported = false;
        std::vector<StateDeclaration> initial_states{};
        std::vector<StateDeclaration> final_states{};
        uint32_t                 first_use  = PassHandle::InvalidIndex;
        uint32_t                 last_use   = PassHandle::InvalidIndex;
    };

    struct PassDeclaration {
        std::string                    name{};
        std::vector<AccessDeclaration> accesses{};
        std::vector<PassHandle>        explicit_dependencies{};
        ExecuteCallback                execute{};
        ExecutionDomain               domain{};
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
        ResourceState    state,
        bool             typed,
        bool             explicit_state,
        std::string_view legacy_range = {}
    );
    void AddStateDeclaration(
        ResourceHandle resource,
        ResourceRange  range,
        ResourceState  state,
        QueueRole      queue,
        AccessMode     boundary_access,
        bool           initial
    );
    bool MarkExported(ResourceHandle resource, bool whole_resource);
    void AddDependency(uint32_t pass_index, PassHandle dependency);
    void MarkSideEffect(uint32_t pass_index);
    void SetExecutionDomain(uint32_t pass_index, QueueRole queue, PipelineType pipeline);
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
    QueueTopology                    queue_topology{};
    bool                             compiled = false;
    bool                             executed = false;
};

} // namespace Moer::Render
