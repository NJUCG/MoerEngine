#pragma once

#include "RenderAPI.h"
#include "misc/NumericLimits.h"
#include "misc/STL.h"
#include "rendergraph/RenderGraphResourcePool.h"
#include "rhi/executor/RHIExecutor.h"

#include <compare>
#include <concepts>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace Moer::Render {

class RenderGraphCompiler;
class RenderGraphLowering;
class RGParameterAccessCollector;
class RenderGraphAsyncSetupTestAccess;
template<typename T>
class RGPreparedValue;

template<typename Input, typename Prepare>
using RGSetupResult = std::remove_cvref_t<std::invoke_result_t<
    std::decay_t<Prepare>&,
    const std::decay_t<Input>&>>;

template<typename T>
concept RGParameterAccessProvider =
    requires(
        const std::remove_cvref_t<T>& value,
        RGParameterAccessCollector&   collector
    ) {
        value.DeclareRGAccess(collector);
    };

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
    static constexpr uint32_t InvalidIndex          = MaxValue<uint32_t>;
    static constexpr uint64_t InvalidGraphId        = MaxValue<uint64_t>;
    static constexpr uint32_t RemainingTextureRange = MaxValue<uint32_t>;
    static constexpr uint64_t RemainingBufferRange  = MaxValue<uint64_t>;
    static constexpr uint32_t InvalidVersion        = InvalidIndex;

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

    enum class SharingMode : uint8_t {
        Exclusive,
        Concurrent,
    };

    struct TextureDesc {
        uint32_t      mip_count   = 1;
        uint32_t      layer_count = 1;
        TextureAspect aspects     = TextureAspect::Color;
        SharingMode sharing_mode=SharingMode::Exclusive;

        friend bool operator==(const TextureDesc&, const TextureDesc&) = default;
    };

    struct BufferDesc {
        uint64_t byte_size = 0;
        SharingMode sharing_mode = SharingMode::Exclusive;

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

        friend auto operator<=>(const TextureRange&, const TextureRange&) = default;
    };

    struct BufferRange {
        uint64_t offset = 0;
        uint64_t size   = RemainingBufferRange;

        [[nodiscard]] static constexpr BufferRange Whole() {
            return {};
        }

        friend auto operator<=>(const BufferRange&, const BufferRange&) = default;
    };

    // A token has no subranges; its monostate is also the default range.
    using ResourceRange = std::variant<std::monostate, TextureRange, BufferRange>;

    [[nodiscard]] static constexpr ResourceKind GetRangeKind(const ResourceRange& range) {
        if (std::holds_alternative<TextureRange>(range)) {
            return ResourceKind::Texture;
        }
        if (std::holds_alternative<BufferRange>(range)) {
            return ResourceKind::Buffer;
        }
        return ResourceKind::Token;
    }

    [[nodiscard]] static constexpr ResourceRange WholeResourceRange(ResourceKind kind) {
        switch (kind) {
            case ResourceKind::Texture:
                return TextureRange::Whole();
            case ResourceKind::Buffer:
                return BufferRange::Whole();
            case ResourceKind::Token:
                return std::monostate{};
        }
        return std::monostate{};
    }

    struct ResourceHandle {
        uint32_t resource_id = InvalidIndex;
        uint64_t graph_id    = InvalidGraphId;

        [[nodiscard]] bool IsValid() const {
            return resource_id != InvalidIndex && graph_id != InvalidGraphId;
        }

        friend bool operator==(ResourceHandle lhs, ResourceHandle rhs) {
            return lhs.resource_id == rhs.resource_id && lhs.graph_id == rhs.graph_id;
        }
    };

    template<ResourceKind Kind>
    struct TypedResourceHandle{
        ResourceHandle handle;

        [[nodiscard]] bool IsValid() const{
            return handle.IsValid();
        }

        [[nodiscard]] ResourceHandle Untyped() const{
            return handle;
        }

        friend bool operator==(const TypedResourceHandle&,const TypedResourceHandle&) = default;

    };

    using TextureHandle= TypedResourceHandle<ResourceKind::Texture>;
    using BufferHandle = TypedResourceHandle<ResourceKind::Buffer>;
    using TokenHandle = TypedResourceHandle<ResourceKind::Token>;

    struct PassHandle {
        uint32_t pass_id  = InvalidIndex;
        uint64_t graph_id = InvalidGraphId;

        [[nodiscard]] bool IsValid() const {
            return pass_id != InvalidIndex && graph_id != InvalidGraphId;
        }

        friend bool operator==(PassHandle lhs, PassHandle rhs) {
            return lhs.pass_id == rhs.pass_id && lhs.graph_id == rhs.graph_id;
        }
    };

    /** Stable identity for one graph-owned CPU setup task. */
    struct SetupPassHandle {
        uint32_t setup_pass_id = InvalidIndex;
        uint64_t graph_id      = InvalidGraphId;

        [[nodiscard]] bool IsValid() const {
            return setup_pass_id != InvalidIndex && graph_id != InvalidGraphId;
        }

        friend bool operator==(SetupPassHandle lhs, SetupPassHandle rhs) {
            return lhs.setup_pass_id == rhs.setup_pass_id && lhs.graph_id == rhs.graph_id;
        }
    };

    enum class SetupPassSafety : uint8_t {
        Restricted,
        Unsafe,
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

    /**
     * GPU work category used to validate pass resource declarations and derive
     * shader access stage scopes. Transfer stages come from resource states.
     */
    enum class GpuWorkType : uint8_t {
        None,
        Graphics,
        Compute,
        RayTracing,
        Transfer,
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
        QueueRole   queue = QueueRole::Graphics;
        GpuWorkType work  = GpuWorkType::Graphics;

        friend bool operator==(const ExecutionDomain&, const ExecutionDomain&) = default;
    };

    /** CPU recording policy; independent of GPU dependencies and native translation. */
    enum class PassRecordPolicy : uint8_t {
        /** Join earlier recording in compiled order, run on the caller, then allow the next group. */
        Serial,
        /** May record concurrently into an exclusively owned CommandList. */
        ParallelAllowed,
    };

    /** Per-access purpose, independent of creation capabilities and read/write mode. */
    enum class TextureAccessType : uint8_t {
        /** Logical-only declarations may omit a type; physical lowering rejects it. */
        Unspecified,
        TransferSource,
        TransferDestination,
        Sampled,
        Storage,
        ColorAttachment,
        DepthStencilAttachment,
    };

    enum class BufferAccessType : uint8_t {
        Unspecified,
        TransferSource,
        TransferDestination,
        VertexBuffer,
        IndexBuffer,
        IndirectArgument,
        ShaderRead,
        Storage,
        AccelerationStructureBuildInput,
        AccelerationStructureRead,
        AccelerationStructureWrite,
    };

    /** Special graph boundaries, never ordinary pass accesses. */
    enum class TextureBoundary : uint8_t {
        Undefined,
        /** Presentation-copy source in GENERAL with transfer-read visibility. */
        PresentationSource,
    };

    enum class BufferBoundary : uint8_t {
        Undefined,
    };

    using TextureBoundaryState = std::variant<TextureAccessType, TextureBoundary>;
    using BufferBoundaryState  = std::variant<BufferAccessType, BufferBoundary>;

    // A token has no GPU state; monostate is also the default state.
    using ResourceState = std::variant<
        std::monostate,
        TextureAccessType,
        BufferAccessType,
        TextureBoundary,
        BufferBoundary>;

    [[nodiscard]] static constexpr ResourceKind GetStateKind(const ResourceState& state) {
        if (std::holds_alternative<TextureAccessType>(state) || std::holds_alternative<TextureBoundary>(state)) {
            return ResourceKind::Texture;
        }
        if (std::holds_alternative<BufferAccessType>(state) || std::holds_alternative<BufferBoundary>(state)) {
            return ResourceKind::Buffer;
        }
        return ResourceKind::Token;
    }

    [[nodiscard]] static constexpr ResourceState UnspecifiedResourceState(ResourceKind kind) {
        switch (kind) {
            case ResourceKind::Texture:
                return TextureAccessType::Unspecified;
            case ResourceKind::Buffer:
                return BufferAccessType::Unspecified;
            case ResourceKind::Token:
                return std::monostate{};
        }
        return std::monostate{};
    }

    [[nodiscard]] static constexpr bool IsUnspecified(const ResourceState& state) {
        if (const auto* texture = std::get_if<TextureAccessType>(&state)) {
            return *texture == TextureAccessType::Unspecified;
        }
        if (const auto* buffer = std::get_if<BufferAccessType>(&state)) {
            return *buffer == BufferAccessType::Unspecified;
        }
        // Tokens have no explicit GPU state; special boundaries do.
        return std::holds_alternative<std::monostate>(state);
    }

    [[nodiscard]] static constexpr bool IsUndefined(const ResourceState& state) {
        if (const auto* texture = std::get_if<TextureBoundary>(&state)) {
            return *texture == TextureBoundary::Undefined;
        }
        if (const auto* buffer = std::get_if<BufferBoundary>(&state)) {
            return *buffer == BufferBoundary::Undefined;
        }
        return false;
    }

    [[nodiscard]] static constexpr bool IsPresentationSource(const ResourceState& state) {
        const auto* boundary = std::get_if<TextureBoundary>(&state);
        return boundary && *boundary == TextureBoundary::PresentationSource;
    }

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
        uint32_t       first_use     = InvalidIndex;
        uint32_t       last_use      = InvalidIndex;
        uint32_t       version_count = 0;
        /** Compiler-assigned whole-object transient allocation slot. */
        uint32_t       transient_slot = InvalidIndex;
        bool           imported      = false;
        bool           exported      = false;
    };

    struct CompiledDependencyLevel {
        std::vector<PassHandle> passes{};
    };

    /**
     * One stable pass identity in compiled order. Execution may place multiple
     * units in one frontend stream; native translation batching remains separate.
     */
    struct CompiledFrontendRecordUnit {
        uint32_t                source_index = 0;
        QueueBinding            target_queue{};
        PassHandle              pass{};
        PassRecordPolicy        record_policy = PassRecordPolicy::Serial;
        uint32_t                recording_workload = 1;
        ERHITranslateExecutionClass native_translate_class{
            ERHITranslateExecutionClass::Parallel
        };

        uint32_t                dependency_level_index = InvalidIndex;
    };

    /**
     * Contiguous frontend recording units that may be dispatched together.
     * GPU resource hazards do not split a group because immutable frontend
     * command streams can be recorded concurrently and assembled later in
     * compiled order. CPU preparation must use Prepare/Record contracts, not
     * GPU graph edges to communicate between recording callbacks.
     */
    struct CompiledFrontendDispatchGroup {
        uint32_t           first_unit = 0;
        uint32_t           unit_count = 0;
        PassRecordPolicy record_policy = PassRecordPolicy::Serial;
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
        ExecutionDomain domain{QueueRole::None, GpuWorkType::None};

        friend bool operator==(const CompiledBarrierSource&, const CompiledBarrierSource&) = default;
    };

    struct CompiledBarrier {
        ResourceHandle  resource{};
        ResourceRange   range{};
        /** Representative predecessor; sources is authoritative. Invalid for an external/undefined boundary. */
        PassHandle      src_pass{};
        PassHandle      dst_pass{};
        /** Current tracked state at the destination boundary, independent of source fan-in. */
        ResourceState   before_state{};
        ResourceState   after_state{};
        /** Representative source scope; sources is authoritative when it is non-empty. */
        AccessMode      before_access = AccessMode::None;
        AccessMode      after_access  = AccessMode::None;
        ExecutionDomain src_domain{QueueRole::None, GpuWorkType::None};
        ExecutionDomain dst_domain{QueueRole::None, GpuWorkType::None};
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
        uint32_t       transient_slot = InvalidIndex;
        ResourceHandle predecessor_resource{};
        ResourceHandle successor_resource{};
        /** Latest source for compact diagnostics; source_frontier is authoritative. */
        PassHandle     primary_src_pass{};
        std::vector<PassHandle> source_frontier{};
        PassHandle     dst_pass{};
        uint32_t       barrier_index = InvalidIndex;
    };

    struct CompiledQueueSync {
        uint32_t     id = 0;
        PassHandle   signal_pass{};
        PassHandle   wait_pass{};
        QueueBinding signal_queue{};
        QueueBinding wait_queue{};
        uint32_t     signal_batch = InvalidIndex;
        uint32_t     wait_batch   = InvalidIndex;
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
     * Immutable compiler output. frontend_record_units is the executable CPU
     * ownership schedule; barriers, queue_batches, and queue_syncs are the
     * active Graphics/Compute/Copy lowering contract. Queue syncs describe
     * managed internal pass/batch edges only. External import/export/present
     * synchronization endpoints remain intentionally absent.
     */
    struct CompiledPlan {
        /** Stable topological execution order. CPU recording policy is described separately below. */
        std::vector<PassHandle> execution_order{};
        /** Minimal hazard, explicit, state, and ownership frontier without fake serial edges. */
        std::vector<CompiledEdge> edges{};
        /** Atomic subresource/range accesses with logical input/output versions. */
        std::vector<CompiledAccess>   accesses{};
        std::vector<CompiledResource> resources{};
        /** Semantic dependency levels for GPU graph analysis, independent of frontend recording. */
        std::vector<CompiledDependencyLevel> dependency_levels{};
        /** Stable one-pass frontend sources with explicit thread-safety classification. */
        std::vector<CompiledFrontendRecordUnit> frontend_record_units{};
        /** CPU-class dispatch groups; GPU dependency edges never split them. */
        std::vector<CompiledFrontendDispatchGroup> frontend_dispatch_groups{};
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
            execution_order.clear();
            edges.clear();
            accesses.clear();
            resources.clear();
            dependency_levels.clear();
            frontend_record_units.clear();
            frontend_dispatch_groups.clear();
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
            TextureHandle     resource,
            TextureAccessType type,
            TextureRange      range = TextureRange::Whole()
        );
        PassBuilder& Write(
            TextureHandle     resource,
            TextureAccessType type,
            TextureRange      range = TextureRange::Whole()
        );
        PassBuilder& ReadWrite(
            TextureHandle     resource,
            TextureAccessType type,
            TextureRange      range = TextureRange::Whole()
        );

        PassBuilder& Read(BufferHandle resource, BufferRange range = BufferRange::Whole());
        PassBuilder& Write(BufferHandle resource, BufferRange range = BufferRange::Whole());
        PassBuilder& ReadWrite(BufferHandle resource, BufferRange range = BufferRange::Whole());
        PassBuilder& Read(
            BufferHandle     resource,
            BufferAccessType type,
            BufferRange      range = BufferRange::Whole()
        );
        PassBuilder& Write(
            BufferHandle     resource,
            BufferAccessType type,
            BufferRange      range = BufferRange::Whole()
        );
        PassBuilder& ReadWrite(
            BufferHandle     resource,
            BufferAccessType type,
            BufferRange      range = BufferRange::Whole()
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

        /** GPU dependency independent of declaration order; must not conflict with resource dependencies. */
        PassBuilder& DependsOn(PassHandle dependency);
        PassBuilder& SideEffect();
        PassBuilder& ExecuteOn(QueueRole queue, GpuWorkType work);
        /** Relative CPU recording work (default 1), typically draw/dispatch count; must be positive. */
        PassBuilder& RecordWorkload(uint32_t units);
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
        PassRecordPolicy record_policy = PassRecordPolicy::Serial;
        ERHITranslateExecutionClass translate_execution_class{
            ERHITranslateExecutionClass::Parallel
        };
    };

    using SetupCallback         = std::function<void(PassBuilder&)>;

    using RecordCallback        = std::function<void(CommandList&)>;

    /**
     * Called once for each actual stream, with all its passes in compiled order.
     * May attach submit metadata; CommandList, producer gate and transaction
     * gate ownership are immutable. It must not wait on RHI work. Signals cover
     * the complete stream; a pass-specific receipt belongs to its containing stream.
     */
    using RecordingSourceSetupCallback =
        std::function<void(std::span<const ExecutedPassInfo>, RHIRecordingSource&)>;
    /**
     * Admission-only callback invoked before the corresponding producers
     * start. It may move the immutable pending sources into a nonblocking
     * handoff, but must not mutate/seal their CommandLists, replace or complete
     * their producer/transaction gates, wait on those gates, or call blocking
     * RHI lifecycle operations such as Sync. Producer completion is joined by
     * ExecuteFrontendRecordingPlan; active lowering releases every published
     * group through one graph-wide commit gate only after the whole graph succeeds.
     */
    using FrontendSourcePublisher =
        std::function<void(Array<RHIRecordingSource>&&)>;

    /**
     * Materializes the compiled queue plan, exact resource ranges and barriers,
     * and allocates transients from the Completion-owned pool. With enabled=false,
     * only logical/frontend recording is performed;
     * such BackendTracked command streams cannot be submitted to Vulkan.
     */
    struct ActiveRecordingOptions {
        bool         enabled                  = false;

        /** Defaults to the global completion-safe pool/allocator. */
        RenderGraphTransientAllocator* transient_allocator = nullptr;
        /** Vulkan accepts only the complete Compiled dependency contract. */
        ERHIResourceStateOwnership state_ownership = ERHIResourceStateOwnership::Compiled;
    };

    /**
     * Optional modern GPU profiling seam for ExecuteFrontendRecordingPlan.
     *
     * Binds once per actual stream, using its first pass's info and compiled
     * source_order. Every pass still gets its own marker in that stream.
     * The graph assigns source_order before any producer is dispatched. A false callback
     * result is a bounded profiling drop: it suppresses legacy timing for that
     * CommandList generation and does not fail graph execution.
     * Throwing, changing CommandList recording state, or returning a value that
     * disagrees with HasGpuScopeRecorder() fails before source publication.
     */
    struct GpuProfilingOptions {
        using TryBindSource = std::function<bool(
            const ExecutedPassInfo&,
            CommandList&,
            RHIQueueBinding,
            uint64 source_order
        )>;

        TryBindSource try_bind_source{};

        uint64        source_order_base         = 0;
    };

    static const ActiveRecordingOptions& DefaultActiveRecordingOptions();
    static const GpuProfilingOptions& DefaultGpuProfilingOptions();

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
     * graph. It is not the execution domain of the external access; external work and
     * synchronization endpoints remain unbound in the shadow plan.
     */
    void SetInitialState(
        TextureHandle        resource,
        TextureBoundaryState state,
        QueueRole            owner_queue,
        AccessMode           last_access = AccessMode::Unknown,
        TextureRange         range       = TextureRange::Whole()
    );
    void SetInitialState(
        BufferHandle        resource,
        BufferBoundaryState state,
        QueueRole           owner_queue,
        AccessMode          last_access = AccessMode::Unknown,
        BufferRange         range       = BufferRange::Whole()
    );
    void Export(
        TextureHandle        resource,
        TextureBoundaryState final_state,
        QueueRole            owner_queue,
        AccessMode           next_access = AccessMode::Read,
        TextureRange         range       = TextureRange::Whole()
    );
    void Export(
        BufferHandle        resource,
        BufferBoundaryState final_state,
        QueueRole           owner_queue,
        AccessMode          next_access = AccessMode::Read,
        BufferRange         range       = BufferRange::Whole()
    );

    /**
     * Schedules one graph-owned, CPU-only preparation callback. Independent
     * setup passes may run concurrently; declared dependencies are completed
     * first. Every setup pass is joined before graph compilation begins and
     * publishes one immutable result for later recording callbacks.
     */
    template<typename Input, typename Prepare>
        requires std::invocable<
                     std::decay_t<Prepare>&,
                     const std::decay_t<Input>&> &&
                 (!std::is_void_v<RGSetupResult<Input, Prepare>>) &&
                 (!std::is_reference_v<
                     std::invoke_result_t<
                         std::decay_t<Prepare>&,
                         const std::decay_t<Input>&>>) &&
                 std::move_constructible<RGSetupResult<Input, Prepare>> &&
                 std::constructible_from<std::decay_t<Input>, Input&&> &&
                 std::constructible_from<std::decay_t<Prepare>, Prepare&&>
    auto AddSetupPass(
        std::string_view name,
        Input&&          immutable_input,
        Prepare&&        prepare,
        std::span<const SetupPassHandle> dependencies = {},
        SetupPassSafety  safety = SetupPassSafety::Restricted
    ) -> RGPreparedValue<RGSetupResult<Input, Prepare>>;

    /** Explicit escape hatch for CPU preparation that must use the RHI. */
    template<typename Input, typename Prepare>
        requires std::invocable<
                     std::decay_t<Prepare>&,
                     const std::decay_t<Input>&> &&
                 (!std::is_void_v<RGSetupResult<Input, Prepare>>) &&
                 (!std::is_reference_v<
                     std::invoke_result_t<
                         std::decay_t<Prepare>&,
                         const std::decay_t<Input>&>>) &&
                 std::move_constructible<RGSetupResult<Input, Prepare>> &&
                 std::constructible_from<std::decay_t<Input>, Input&&> &&
                 std::constructible_from<std::decay_t<Prepare>, Prepare&&>
    auto AddUnsafeSetupPass(
        std::string_view name,
        Input&&          immutable_input,
        Prepare&&        prepare,
        std::span<const SetupPassHandle> dependencies = {}
    ) -> RGPreparedValue<RGSetupResult<Input, Prepare>> {
        return AddSetupPass(
            name,
            std::forward<Input>(immutable_input),
            std::forward<Prepare>(prepare),
            dependencies,
            SetupPassSafety::Unsafe
        );
    }

    /** The caller explicitly declares CPU recording safety; it is not inferred from GPU access. */
    PassHandle AddRecordPass(
        std::string_view     name,
        const SetupCallback& setup,
        RecordCallback       record,
        PassRecordPolicy policy
    );

    /**
     * Declares a command-recording pass from one graph-owned immutable
     * parameter object. DeclareRGAccess() expands the typed access fields into
     * this graph's existing PassBuilder contract; policy_setup remains the
     * explicit home for queue, GPU work type and side-effect policy.
     */
    template<RGParameterAccessProvider Parameters, typename Record>
        requires std::invocable<
            Record&,
            CommandList&,
            const std::remove_cvref_t<Parameters>&>
    PassHandle AddRecordPass(
        std::string_view name,
        Parameters&&     parameters,
        SetupCallback    policy_setup,
        Record&&         record,
        PassRecordPolicy policy
    );

    /**
     * Compiles declarations without emitting barriers or recording RHI commands.
     * Resource versions follow declaration order; execution follows the compiled dependencies.
     */
    bool Compile();

    /**
     * Records compatible consecutive passes into shared private streams and
     * publishes sources in compiled order. GPU hazards do not order CPU recording; Serial policy joins the
     * previous group before executing on the caller. Every producer is joined
     * before returning, including failures.
     */
    bool ExecuteFrontendRecordingPlan(
        const RecordingSourceSetupCallback& configure_recording_source = {},
        bool                                parallel_recording_enabled = true,
        const FrontendSourcePublisher&      publish_frontend_sources = {},
        const ActiveRecordingOptions&        active_recording = DefaultActiveRecordingOptions(),
        const GpuProfilingOptions&            gpu_profiling = DefaultGpuProfilingOptions()
    );

    /**
     * Records serial regions into shared private CommandLists, partitions
     * parallel regions by recording workload, joins all producers, then appends those frontend
     * streams to one caller-owned Graphics CommandList in compiled order.
     * The destination's later Submit() is the only native submission boundary.
     * A Compiled destination consumes the full lowered barrier plan, including
     * import/export boundaries, and bypasses backend dependency inference.
     * BackendTracked remains available only for unmigrated callers.
     *
     * This is the direct path for Graphics passes backed by
     * already-created resources. It uses the compiler's frontend dispatch
     * groups and never publishes per-pass RHIRecordingSources. Active transient
     * lowering and multi-queue execution remain on
     * ExecuteFrontendRecordingPlan's RHI handoff path.
     */
    bool RecordAndMergeFrontendCommands(
        CommandList&               command_list,
        bool                       parallel_recording_enabled = true,
        const GpuProfilingOptions& gpu_profiling = DefaultGpuProfilingOptions()
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
    friend class RenderGraphAsyncSetupTestAccess;

    enum class SetupFaultForTesting : uint8_t {
        None                 = 0,
        BatchOwnerCreate     = 1 << 0,
        BatchRuntimeCreate   = 1 << 1,
        TaskDispatchThrows   = 1 << 2,
        FailureDiagnostic    = 1 << 3,
    };

    enum class GpuProfileBindOutcome : uint8_t {
        Bound,
        Dropped,
        Failed,
    };

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

        friend bool operator==(const StateDeclaration&, const StateDeclaration&) = default;
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
        uint32_t                 first_use  = InvalidIndex;
        uint32_t                 last_use   = InvalidIndex;
    };

    struct PassDeclaration {
        std::string                    name{};
        std::vector<AccessDeclaration> accesses{};
        std::vector<ResourceHandle>    references{};
        std::vector<PassHandle>        explicit_dependencies{};
        RecordCallback                 record{};
        ExecutionDomain               domain{};
        bool                           side_effect = false;
        PassRecordPolicy               record_policy = PassRecordPolicy::Serial;
        uint32_t                       recording_workload = 1;
        ERHITranslateExecutionClass     translate_execution_class{
            ERHITranslateExecutionClass::Parallel
        };
    };

    struct SetupPassDeclaration {
        std::string                              name{};
        std::vector<uint32_t>                    dependencies{};
        SetupPassSafety                          safety = SetupPassSafety::Restricted;
        std::function<void()>                    execute{};
        std::function<void(std::string_view)>    fail{};
        std::function<void(std::string_view)>    annotate_failure{};
    };

    struct SetupBatchState;
    class FrontendRecordMergeExecutor;

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
    void SetExecutionDomain(uint32_t pass_index, QueueRole queue, GpuWorkType work);
    void SetPassTranslateExecutionClass(
        uint32_t                    pass_index,
        ERHITranslateExecutionClass execution_class
    );
    SetupPassHandle RegisterSetupPass(
        std::string_view                         setup_name,
        std::span<const SetupPassHandle>         dependencies,
        SetupPassSafety                         safety,
        std::function<void()>                    execute,
        std::function<void(std::string_view)>    fail,
        std::function<void(std::string_view)>    annotate_failure
    );
    void DispatchSetupPassesAsync();
    void FailUndispatchedSetupPasses(std::string_view reason) noexcept;
    bool WaitSetupPasses(std::string& error) noexcept;
    bool InvalidateCompile();
    bool FailCompile(std::string message);

    [[nodiscard]] ExecutedPassInfo MakeExecutedPassInfo(
        PassHandle pass
    ) const;
    GpuProfileBindOutcome BindGpuProfileSource(
        const GpuProfilingOptions& options,
        const ExecutedPassInfo&    pass_info,
        CommandList&               command_list,
        RHIQueueBinding            queue_binding,
        uint64                     source_order
    );

    [[nodiscard]] bool IsValidResource(ResourceHandle resource) const;
    [[nodiscard]] bool IsValidPass(PassHandle pass) const;

    struct ResourceNameHash {
        using is_transparent = void;

        size_t operator()(std::string_view value) const noexcept {
            return std::hash<std::string_view>{}(value);
        }
    };

    std::string                      name{};
    std::vector<ResourceDeclaration> resources{};
    // Graph-local declaration indexes. Names include aliases; only imports
    // have a physical identity while declarations remain mutable.
    std::unordered_map<std::string, uint32_t, ResourceNameHash, std::equal_to<>> resource_names{};
    std::unordered_map<const void*, uint32_t> imported_resource_ids{};
    std::vector<PassDeclaration>     passes{};
    std::vector<SetupPassDeclaration> setup_passes{};
    SharedPtr<SetupBatchState>        setup_batch{};
    CompiledPlan                     compiled_plan{};
    std::vector<std::string>         declaration_errors{};
    std::string                      compile_error{};
    uint64_t                         graph_id = InvalidGraphId;
    QueueTopology                    queue_topology{};
    bool                             compiled = false;
    bool                             executed = false;
    bool                             setup_dispatched = false;
    bool                             setup_failed_before_batch = false;
    uint8_t                          setup_faults_for_testing = 0;
};

} // namespace Moer::Render

#include "RenderGraphPassParameters.h"
#include "RenderGraphSetup.h"
