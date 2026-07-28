#pragma once

#include "RenderAPI.h"
#include "rendergraph/RenderGraphResourcePool.h"
#include "rhi/RHIExecutor.h"

#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Moer::Render {

class RenderGraphCompiler;
class RenderGraphLowering;

/**
 * Typed RDG frontend with an opt-in command-recording schedule.
 *
 * The compiler owns typed resource declarations, subresource-aware hazards,
 * logical resource versions, stable dependency analysis and lifetimes. Its
 * default execution remains backend-tracked for compatibility; the guarded
 * active path lowers a complete supported state plan into authoritative RHI
 * barriers. Legacy callbacks stay serial. Explicit record callbacks may be
 * assigned independent CommandLists while preserving stable source order.
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
        bool      available       = true;

        friend bool operator==(const QueueBinding&, const QueueBinding&) = default;
    };

    /**
     * Maps logical roles to actual queues. Tests may provide a fake topology;
     * active production graphs should snapshot QueueTopology::FromRHI().
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

        /** Snapshot the initialized RHI's real native queue/family mapping. */
        [[nodiscard]] RENDER_API static QueueTopology FromRHI();

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

    /** CPU callback/CommandList ownership; distinct from backend translate policy. */
    enum class PassExecutionClass : uint8_t {
        /** Caller-thread callback whose commands are sealed by the graph observer. */
        MainThread,
        /**
         * Caller-thread, CPU-only preparation/history callback. It may order
         * Token resources or Reference GPU identities, never seals commands,
         * and is excluded from the prospective GPU queue plan.
         */
        CpuPrepare,
        /**
         * Caller-thread hard boundary that owns its command/submission scope.
         * Used for external Vulkan/CUDA synchronization and unmanaged submission.
         */
        ExternalControl,
        SerialRecord,
        ParallelRecordEligible,
    };

    /** RDG-neutral states; these do not depend on the legacy RHI enum ordinals. */
    enum class TextureState : uint8_t {
        Automatic,
        Undefined,
        TransferSource,
        /**
         * External presentation-copy source. This boundary-only state keeps
         * the image in COMMON/GENERAL while establishing transfer-read
         * visibility for the presentation queue.
         */
        PresentationSource,
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
        /** State/data availability established by a prior pass. */
        StateTransition,
        QueueOwnership,
        /**
         * Two non-overlapping logical transient resources reuse one physical
         * whole-object allocation. This is an execution dependency, not a CPU
         * recording dependency: immutable command lists may still be recorded
         * in parallel and are submitted in compiled order.
         */
        TransientAlias,
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
        /** Compiler-assigned whole-object transient allocation slot. */
        uint32_t       transient_slot = PassHandle::InvalidIndex;
        bool           imported      = false;
        bool           exported      = false;
    };

    struct CompiledWave {
        std::vector<PassHandle> passes{};
    };

    /**
     * Stable CPU recording unit. The first implementation deliberately keeps
     * one pass per batch; coalescing is a later optimization and must not alter
     * callback ownership or source order.
     */
    struct CompiledRecordingBatch {
        uint32_t                id = 0;
        QueueBinding            queue{};
        std::vector<PassHandle> passes{};
        PassExecutionClass      execution = PassExecutionClass::SerialRecord;
        ERHITranslateExecutionClass translate_execution_class{
            ERHITranslateExecutionClass::Parallel
        };
        uint32_t                workload = 1;
        uint32_t                dependency_wave = PassHandle::InvalidIndex;
    };

    /**
     * One canonical synchronization decision for one atomic resource cell.
     * Lowering materializes queue_ownership as one paired release/acquire;
     * RDG compilation itself remains backend-neutral.
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
        /** Whole-object transient reuse boundary between two logical resources. */
        bool             transient_alias = false;
        /** Authoritative synchronization frontier whose scopes must be represented by lowering. */
        std::vector<CompiledBarrierSource> sources{};
    };

    /**
     * Compiler-selected reuse of one descriptor-exact physical transient slot.
     *
     * This models whole RHI-object reuse. It is intentionally distinct from
     * placed-resource / shared-heap aliasing.
     */
    struct CompiledAliasBoundary {
        uint32_t       transient_slot = PassHandle::InvalidIndex;
        ResourceHandle predecessor_resource{};
        ResourceHandle successor_resource{};
        /** Latest source for compact diagnostics; source_frontier is authoritative. */
        PassHandle     primary_src_pass{};
        std::vector<PassHandle> source_frontier{};
        PassHandle     dst_pass{};
        uint32_t       barrier_index = PassHandle::InvalidIndex;
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
         * An externally managed submit/synchronization scope. It is always a
         * standalone batch so managed lowering cannot place commands or
         * barriers across the external-control boundary.
         */
        bool                    external_control = false;
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
     * Immutable compiler output. recording_batches is the executable CPU
     * ownership schedule; barriers, queue_batches, and queue_syncs are the
     * active Graphics/Compute/Copy lowering contract. Queue syncs describe
     * managed internal pass/batch edges only. External import/export/present
     * synchronization endpoints remain intentionally absent.
     */
    struct CompiledPlan {
        /** Stable topological order of semantic dependencies. */
        std::vector<PassHandle> topological_order{};
        /** Stable production order. CPU recording policy is described separately below. */
        std::vector<PassHandle> execution_order{};
        /** Minimal hazard, explicit, state, and ownership frontier without fake serial edges. */
        std::vector<CompiledEdge> edges{};
        /** Atomic subresource/range accesses with logical input/output versions. */
        std::vector<CompiledAccess>   accesses{};
        std::vector<CompiledResource> resources{};
        /** Semantic dependency waves; CPU recording groups apply their own edge taxonomy. */
        std::vector<CompiledWave> dependency_waves{};
        /** Stable one-pass CPU recording batches with explicit thread-safety classification. */
        std::vector<CompiledRecordingBatch> recording_batches{};
        /** Canonical state/memory/ownership decisions, prior to backend-specific lowering. */
        std::vector<CompiledBarrier> barriers{};
        /** Descriptor-exact, non-overlapping whole-object transient reuse boundaries. */
        std::vector<CompiledAliasBoundary> alias_boundaries{};
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
            recording_batches.clear();
            barriers.clear();
            alias_boundaries.clear();
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

        /**
         * Declares that a CPU callback observes only the resource identity and
         * extends its logical graph lifetime. Reference does not itself own the
         * physical object or declare a GPU content access, version, barrier, or
         * queue dependency; the callback must capture an owning ref and record
         * passes must still declare their real GPU reads.
         */
        PassBuilder& Reference(ResourceHandle resource);
        PassBuilder& Reference(TextureHandle resource) {
            return Reference(resource.Untyped());
        }
        PassBuilder& Reference(BufferHandle resource) {
            return Reference(resource.Untyped());
        }
        PassBuilder& Reference(TokenHandle resource) {
            return Reference(resource.Untyped());
        }

        PassBuilder& DependsOn(PassHandle dependency);
        PassBuilder& SideEffect();
        PassBuilder& ExecuteOn(QueueRole queue, PipelineType pipeline);
        PassBuilder& MainThread();
        PassBuilder& CpuPrepare();
        PassBuilder& ExternalControl();
        PassBuilder& SerialRecord(uint32_t workload = 1);
        PassBuilder& ParallelRecord(uint32_t workload = 1);
        /**
         * Makes backend translation of this recorded source a stable CPU
         * frontier. The callback still owns an ordinary graph CommandList;
         * this policy is for external mutable integrations that must not
         * translate concurrently with earlier or later sources.
         */
        PassBuilder& TranslateSerialControl();

    private:
        PassBuilder(RenderGraph& graph, uint32_t pass_index) : graph(graph), pass_index(pass_index) {}

        RenderGraph& graph;
        uint32_t     pass_index;

        friend class RenderGraph;
    };

    struct ExecutedPassInfo {
        PassHandle       handle{};
        std::string_view name{};
        ExecutionDomain domain{};
        bool             side_effect{false};
        PassExecutionClass execution_class = PassExecutionClass::MainThread;
        ERHITranslateExecutionClass translate_execution_class{
            ERHITranslateExecutionClass::Parallel
        };
    };

    using SetupCallback         = std::function<void(PassBuilder&)>;
    using ExecuteCallback       = std::function<void()>;
    using RecordCallback        = std::function<void(CommandList&)>;
    using PassCompletedCallback = std::function<void(const ExecutedPassInfo&)>;
    /**
     * May attach submit metadata; CommandList, producer gate and transaction
     * gate ownership are immutable. It must not wait on RHI work.
     */
    using RecordingSourceSetupCallback =
        std::function<void(const ExecutedPassInfo&, RHIRecordingSource&)>;
    /**
     * Admission-only callback invoked before the corresponding producers
     * start. It may move the immutable pending sources into a nonblocking
     * handoff, but must not mutate/seal their CommandLists, replace or complete
     * their producer/transaction gates, wait on those gates, or call blocking
     * RHI lifecycle operations such as Sync. Producer completion is joined by
     * ExecuteRecording; active lowering releases every published group through
     * one graph-wide commit gate only after the whole graph succeeds.
     */
    using RecordingBatchPublisher = std::function<void(Array<RHIRecordingSource>&&)>;

    /**
     * Opts ExecuteRecording into the authoritative RDG state path.
     *
     * Active lowering materializes a fully validated Graphics-only plan into
     * explicit RHI barriers and allocates descriptor-backed transients from a
     * Completion-safe pool. Main-thread passes record into the caller-owned
     * list; independently recorded passes continue to own their own lists and
     * are mutation-sealed until one graph-wide RHI transaction commits. The
     * current backend-tracker bridge requires full-buffer ranges, all physical
     * texture aspects, and an initially empty main-thread list. Active lowering
     * fails closed when caller-thread and managed-record passes are mixed in
     * one graph. A physical MainThread graph must be isolated from nonphysical
     * passes and keep its caller-owned list unsealed until ExecuteRecording
     * returns (therefore no per-pass completion observer). Leaving enabled
     * false preserves the legacy backend-tracked path and rejects active
     * allocation-backed transient declarations.
     */
    struct ActiveRecordingOptions {
        bool         enabled                  = false;
        CommandList* main_thread_command_list = nullptr;
        /** Defaults to the global completion-safe pool/allocator. */
        RenderGraphTransientAllocator* transient_allocator = nullptr;
    };

    /**
     * Optional modern GPU profiling seam for ExecuteRecording.
     *
     * The graph assigns source_order from the immutable compiled execution
     * schedule before any managed producer is dispatched. A false callback
     * result is a bounded profiling drop: it suppresses legacy timing for that
     * CommandList generation and does not fail graph execution.
     * Throwing, changing CommandList recording state, or returning a value that
     * disagrees with HasGpuScopeRecorder() fails before source publication.
     * MainThread passes reuse one binding while the caller-owned CommandList
     * seal generation is unchanged and no managed GPU source intervenes.
     * Before any managed GPU source is bound or published after a MainThread
     * source, the completion observer must rotate the caller-owned generation
     * and leave its replacement empty, unbound, and unsuppressed. The first
     * pass of each new generation binds a new source. If a MainThread callback
     * fails after recording begins, the caller still owns that partial
     * generation and must reject/rotate it before recording an unrelated tail.
     */
    struct GpuProfilingOptions {
        using TryBindSource = std::function<bool(
            const ExecutedPassInfo&,
            CommandList&,
            RHIQueueBinding,
            uint64 source_order
        )>;

        TryBindSource try_bind_source{};
        /** Required for MainThread passes when try_bind_source is present. */
        CommandList*  main_thread_command_list = nullptr;
        uint64        source_order_base         = 0;
    };

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
    /**
     * Strong physical imports used by active lowering. The graph keeps the
     * resource alive through compilation, recording and lowered-plan handoff.
     * The raw-pointer overloads above remain declaration-only compatibility
     * APIs and are deliberately rejected by active lowering.
     */
    TextureHandle ImportTexture(std::string_view name, TextureRef texture, TextureDesc desc);
    BufferHandle  ImportBuffer(std::string_view name, BufferRef buffer, BufferDesc desc);
    TokenHandle   ImportToken(std::string_view name, const void* physical_identity = nullptr);

    /** Legacy transient API. Prefer the typed declarations below for new code. */
    ResourceHandle CreateTransient(std::string_view name, ResourceKind kind);
    TextureHandle  CreateTransientTexture(std::string_view name, TextureDesc desc);
    BufferHandle   CreateTransientBuffer(std::string_view name, BufferDesc desc);
    /**
     * Allocation-backed transient declarations. The compiler shape is derived
     * from the physical descriptor so range validation and pool keys cannot
     * drift apart.
     */
    TextureHandle CreateTransientTexture(
        std::string_view               name,
        const RGTransientTextureDesc& allocation_desc
    );
    BufferHandle CreateTransientBuffer(
        std::string_view              name,
        const RGTransientBufferDesc& allocation_desc
    );
    TokenHandle    CreateTransientToken(std::string_view name);

    /**
     * Valid during the declaring pass callback after active allocation.
     * Retaining a physical ref beyond that callback is an RDG lifetime escape;
     * declare PassBuilder::Reference wherever the object must remain live.
     */
    [[nodiscard]] TextureRef GetPhysicalTexture(TextureHandle resource) const;
    [[nodiscard]] BufferRef  GetPhysicalBuffer(BufferHandle resource) const;

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

    /**
     * Boundary owner_queue identifies the queue family that owns the resource before/after the
     * graph. It is not the execution domain of the external access; external pipeline and
     * synchronization endpoints remain unbound in the shadow plan.
     */
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
    PassHandle AddRecordPass(
        std::string_view     name,
        const SetupCallback& setup,
        RecordCallback       record,
        PassExecutionClass   execution = PassExecutionClass::SerialRecord,
        uint32_t             workload = 1
    );

    /** Compiles declarations without emitting barriers or recording RHI commands. */
    bool Compile();

    /** Executes callbacks synchronously and serially once in compiled order. */
    bool Execute();

    /**
     * Executes callbacks serially and invokes the observer after each callback
     * has completely returned. The observer can safely seal the commands just
     * recorded by that pass without allowing GPU scopes to cross a submission.
     */
    bool Execute(const PassCompletedCallback& after_pass);

    /**
     * Executes legacy callbacks on the caller and explicit record callbacks on
     * independently owned CommandLists. Contiguous eligible passes on one queue
     * are dispatched together even when texture/buffer GPU hazards place them
     * in different dependency waves: those hazards constrain submission, not
     * immutable CPU command recording. Token hazards and explicit DependsOn
     * edges remain CPU recording boundaries. Sources are registered with RHI
     * in compiled order and joined before the next caller-thread pass.
     */
    bool ExecuteRecording(
        const PassCompletedCallback&        after_main_thread_pass,
        const RecordingSourceSetupCallback& configure_recording_source = {},
        bool                                parallel_recording_enabled = true,
        const RecordingBatchPublisher&      publish_recording_batch = {},
        const ActiveRecordingOptions&        active_recording = {},
        const GpuProfilingOptions&            gpu_profiling = {}
    );

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
    friend class RenderGraphLowering;
    friend class RenderGraphTransientAllocator;

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
        TextureRef               physical_texture{};
        BufferRef                physical_buffer{};
        TextureDesc              texture_desc{};
        BufferDesc               buffer_desc{};
        std::optional<RGTransientTextureDesc> transient_texture_desc{};
        std::optional<RGTransientBufferDesc>  transient_buffer_desc{};
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
        std::vector<ResourceHandle>    references{};
        std::vector<PassHandle>        explicit_dependencies{};
        ExecuteCallback                execute{};
        RecordCallback                 record{};
        ExecutionDomain               domain{};
        bool                           side_effect = false;
        PassExecutionClass             execution_class = PassExecutionClass::MainThread;
        ERHITranslateExecutionClass     translate_execution_class{
            ERHITranslateExecutionClass::Parallel
        };
        uint32_t                       workload = 1;
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
    void ReleaseNonExportedTransientBindings() noexcept;
    void AddDependency(uint32_t pass_index, PassHandle dependency);
    void AddReference(uint32_t pass_index, ResourceHandle resource);
    void MarkSideEffect(uint32_t pass_index);
    void SetExecutionDomain(uint32_t pass_index, QueueRole queue, PipelineType pipeline);
    void SetPassExecutionClass(
        uint32_t           pass_index,
        PassExecutionClass execution_class,
        uint32_t           workload
    );
    void SetPassTranslateExecutionClass(
        uint32_t                    pass_index,
        ERHITranslateExecutionClass execution_class
    );
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
