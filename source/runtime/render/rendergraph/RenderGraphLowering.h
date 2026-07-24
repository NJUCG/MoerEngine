#pragma once

#include "RenderAPI.h"
#include "rendergraph/RenderGraph.h"
#include "rhi/RHICommon.h"

#include <span>
#include <string>
#include <vector>
#include <limits>

namespace Moer::Render {

/**
 * CPU-only lowering of a complete RenderGraph compiler plan.
 *
 * Active lowering intentionally supports a narrow, auditable subset: strongly
 * bound imported or allocation-backed transient resources, explicit states at
 * every physical access and external boundary, and the Graphics logical queue
 * only. Unsupported plans fail closed before producing any instructions. RHI
 * command materialization is a separate step.
 */
class RENDER_API RenderGraphLowering {
public:
    static constexpr uint32_t InvalidBarrierIndex = std::numeric_limits<uint32_t>::max();
    static constexpr uint32_t InvalidAccessIndex  = std::numeric_limits<uint32_t>::max();

    enum class InstructionKind : uint8_t {
        Barrier,
        /**
         * Adopts the explicit input state of a pass whose same-state access did
         * not require a compiler barrier. Materializers may emit a no-op
         * explicit barrier; its purpose is persistent tracker state adoption
         * across independently submitted CommandLists.
         */
        StateSeed,
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
        /** Index in RenderGraph::CompiledPlan::barriers, or InvalidBarrierIndex for a seed. */
        uint32_t correlation_id = 0;
        uint32_t barrier_index  = InvalidBarrierIndex;
        /** Index in CompiledPlan::accesses for StateSeed, otherwise InvalidAccessIndex. */
        uint32_t access_index   = InvalidAccessIndex;

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

    /**
     * Produces a complete plan or no plan. On failure output is cleared and
     * error contains the first deterministic unsupported-condition diagnostic.
     */
    [[nodiscard]] static bool
    Lower(const RenderGraph& graph, LoweredPlan& output, std::string& error);
};

} // namespace Moer::Render
