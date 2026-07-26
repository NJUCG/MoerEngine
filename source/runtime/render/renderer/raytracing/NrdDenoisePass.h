#pragma once

#include "RaytracingGraphResources.h"
#include "rhi/plugin/NrdPlugin.h"

namespace Moer::Render::Raytracing {

#if WITH_NRD

/**
 * NRD is an external mutable SDK, but its renderer-facing pass is immutable.
 * Prepare freezes one accepted-history candidate and every TextureRef. The
 * graph callback only appends that packet; SDK state changes happen later at
 * a SerialControl translation frontier.
 */
class NrdDenoisePass {
public:
    struct PreparedCommand {
        SharedPtr<Ext::NRDInterface>         interface{};
        Ext::NRDInterface::PreparedFrameRef  frame{};
        FenceRef                             submission_fence{};
        uint64                               submission_value = 1;
        FenceRef                             graph_submission_fence{};

        [[nodiscard]] bool IsValid() const {
            return interface && frame && frame->IsValid() &&
                   submission_fence && submission_value != 0 &&
                   graph_submission_fence;
        }
    };

    [[nodiscard]] static PreparedCommand Prepare(
        SharedPtr<Ext::NRDInterface> interface,
        uint32                       frame_index,
        const Vector2ui&             size,
        const Vector2f&              jitter,
        const Matrix4x4f&            view,
        const Matrix4x4f&            projection,
        nrd::Denoiser                denoiser,
        const RTContext&             rt_ctx
    );

    static bool AddPasses(
        RenderGraph&                graph,
        RTGraphFrameResources&      graph_resources,
        const PreparedCommand&      command,
        bool                        outputs_initialized
    );

    static void Process(
        CommandList&           cmd_list,
        const PreparedCommand& command
    );

    /**
     * Waits only for native queue acceptance, not GPU completion. Both the
     * NRD-owning source and the final frame tail must be accepted before
     * renderer-side temporal history advances.
     */
    [[nodiscard]] static bool CommitAcceptedSubmission(
        const PreparedCommand& command,
        const FenceRef&        frame_fence,
        uint64                 frame_value,
        uint64                 graph_submission_value_count
    );
};

#endif

} // namespace Moer::Render::Raytracing
