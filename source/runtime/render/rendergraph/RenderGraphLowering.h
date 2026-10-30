#pragma once

#include "RenderAPI.h"
#include "misc/Types.h"
#include "rendergraph/RenderGraph.h"
#include "rhi/RHICommon.h"

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Moer::Render {

/**
 * CPU-only lowering of a complete RenderGraph compiler plan.
 *
 * Active lowering intentionally supports a narrow, auditable subset: strongly
 * bound imported or allocation-backed transient resources and explicit states
 * at every physical access and external boundary. Graphics/Compute/Copy queue
 * DAGs and internal exclusive queue-family transfers are executable;
 * unmanaged external ownership endpoints remain fail-closed.
 */
class RENDER_API RenderGraphLowering {
public:
    static constexpr uint32 InvalidBarrierIndex = RenderGraph::InvalidIndex;

    enum class InstructionKind : uint8 {
        Barrier,
        /** Source-family half emitted after the designated producer batch. */
        QueueRelease,
        /** Destination-family half emitted before the consuming pass. */
        QueueAcquire,
    };

    struct Scope {
        ERHIPipelineStageFlags stages = ERHIPipelineStageFlags::PS_NONE;
        ERHIAccessFlags        access = ERHIAccessFlags::UNDEFINED;
        ETextureLayout         layout = ETextureLayout::TEXTURE_LAYOUT_UNDEFINED;
        bool                   has_texture_layout = false;
    };

    struct PhysicalBinding {
        RenderGraph::ResourceHandle resource{};
        RenderGraph::ResourceKind   kind = RenderGraph::ResourceKind::Token;
        TextureRef                  texture{};
        BufferRef                   buffer{};

        [[nodiscard]] const void* Identity() const {
            if (kind == RenderGraph::ResourceKind::Texture) {
                return texture.Get();
            }
            if (kind == RenderGraph::ResourceKind::Buffer) {
                return buffer.Get();
            }
            return nullptr;
        }
    };

    struct LoweredInstruction {
        InstructionKind instruction_kind = InstructionKind::Barrier;
        /** Index in RenderGraph::CompiledPlan::barriers. */
        uint32 correlation_id = 0;
        uint32 barrier_index  = InvalidBarrierIndex;

        RenderGraph::ResourceHandle resource{};
        RenderGraph::ResourceKind   resource_kind = RenderGraph::ResourceKind::Token;
        RenderGraph::ResourceRange  range{};
        ETextureAspectFlags         texture_aspects = ETextureAspectFlags::NONE;
        PhysicalBinding             physical{};

        RenderGraph::PassHandle src_pass{};
        RenderGraph::PassHandle dst_pass{};
        std::vector<RenderGraph::PassHandle> source_frontier{};

        Scope source{};
        Scope destination{};
        RenderGraph::ResourceState before_state{};
        RenderGraph::ResourceState after_state{};
        RenderGraph::AccessMode    before_access = RenderGraph::AccessMode::None;
        RenderGraph::AccessMode    after_access  = RenderGraph::AccessMode::None;

        bool state_transition          = false;
        bool memory_dependency         = false;
        bool execution_dependency      = false;
        bool discard_previous_contents = false;
        bool import_boundary           = false;
        bool export_boundary           = false;
        bool transient_alias           = false;
        /** Destination-side visibility/layout barrier after a GPU queue wait. */
        bool queue_acquire              = false;
        /** Canonical logical endpoints shared by paired release/acquire halves. */
        RenderGraph::QueueBinding transfer_source{};
        RenderGraph::QueueBinding transfer_destination{};
    };

    struct QueueSyncInstruction {
        uint32                    correlation_id = 0;
        RenderGraph::PassHandle signal_pass{};
        RenderGraph::PassHandle wait_pass{};
        RenderGraph::QueueBinding signal_queue{};
        RenderGraph::QueueBinding wait_queue{};
        uint32                    signal_batch = RenderGraph::InvalidIndex;
        uint32                    wait_batch   = RenderGraph::InvalidIndex;
        /** Added by lowering to join a fan-in frontier at one release owner. */
        bool synthetic_ownership_join = false;
        std::vector<uint32> ownership_join_barriers{};
        /** Ownership transfers whose release->acquire ordering uses this sync. */
        std::vector<uint32> ownership_transfer_barriers{};
    };

    struct PassInstructions {
        RenderGraph::PassHandle              pass{};
        std::vector<PhysicalBinding>         keepalive{};
        std::vector<LoweredInstruction>      before{};
        std::vector<LoweredInstruction>      after{};
    };

    struct RENDER_API LoweredPlan {
        /**
         * Import-boundary instructions remain separate. A materializer attaches
         * each instruction to its dst_pass before recording that pass.
         */
        std::vector<LoweredInstruction> prologue{};
        /** Stable RenderGraph execution order, excluding CPU-only prepare passes. */
        std::vector<PassInstructions> passes{};
        /** Cross-native-queue synchronization points consumed by RHI metadata. */
        std::vector<QueueSyncInstruction> queue_syncs{};

        void Clear();

        [[nodiscard]] const PassInstructions*
        FindPass(RenderGraph::PassHandle pass) const;
        [[nodiscard]] std::span<const LoweredInstruction>
        Before(RenderGraph::PassHandle pass) const;
        [[nodiscard]] std::span<const LoweredInstruction>
        After(RenderGraph::PassHandle pass) const;
        [[nodiscard]] std::span<const PhysicalBinding>
        Keepalive(RenderGraph::PassHandle pass) const;

        /** Pointer-free deterministic diagnostic representation. */
        [[nodiscard]] std::string Dump() const;
    };

    /** Validates and materializes an instruction. Success with no output means
     *  a read-only boundary needs no GPU command; its proof remains in the plan. */
    [[nodiscard]] static bool MaterializeInstruction(
        const LoweredInstruction& instruction, std::optional<BarrierCreateInfo>& output, std::string& error);

    /**
     * Produces a complete plan or no plan. On failure output is cleared and
     * error contains the first deterministic unsupported-condition diagnostic.
     */
    [[nodiscard]] static bool
    Lower(const RenderGraph& graph, LoweredPlan& output, std::string& error);
};

} // namespace Moer::Render
