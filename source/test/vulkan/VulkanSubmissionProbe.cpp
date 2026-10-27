#include "VulkanSubmissionProbe.h"
#include "rhi/RHI.h"
#include "rhi/backends/vulkan/VulkanQueue.h"
#include "rhi/backends/vulkan/VulkanSubmissionMaterialization.h"
#include "rhi/backends/vulkan/VulkanTimeline.h"

#include <stdexcept>

namespace Moer::Render {
using namespace VulkanSubmissionDetail;
namespace {

class VulkanMaterializationProbeCommand final : public Command {
public:
    explicit VulkanMaterializationProbeCommand(EQueueType _queue) :
        Command(EType::Custom, "VulkanMaterializationProbe"),
        queue(_queue) {}

    EQueueType GetQueueType() const override {
        return queue;
    }

private:
    EQueueType queue{EQueueType::Graphics};
};

} // namespace

VulkanMultiSegmentCompletionProbeResult RunVulkanMultiSegmentCompletionProbeForTesting() {
    Array<FenceRef> segment_outcomes{};
    segment_outcomes.reserve(2);
    segment_outcomes.emplace_back(RenderDevice::Get().CreateFence());
    segment_outcomes.emplace_back(RenderDevice::Get().CreateFence());
    if (!segment_outcomes[0].IsValid() || !segment_outcomes[1].IsValid()) {
        throw std::runtime_error("multi-segment completion probe failed to create fences");
    }

    auto   completion = std::make_shared<VulkanMultiSegmentCompletionState>(std::move(segment_outcomes));
    uint32 ordinary_callback_count = 0;
    uint32 success_callback_count  = 0;
    completion->AdoptSourceCallbacks(
        Array<std::function<void()>>{[&ordinary_callback_count] {
            ++ordinary_callback_count;
        }},
        Array<std::function<void()>>{[&success_callback_count] {
            ++success_callback_count;
        }}
    );

    VulkanMultiSegmentCompletionProbeResult result{};
    auto*                                   suffix = ResourceCast(completion->SegmentOutcome(1).Get());
    auto*                                   prefix = ResourceCast(completion->SegmentOutcome(0).Get());
    if (suffix == nullptr || prefix == nullptr) {
        throw std::runtime_error("multi-segment completion probe received non-Vulkan fences");
    }

    suffix->Fail(VK_ERROR_UNKNOWN);
    completion->RetireOrdinary(1);
    result.suffix_retirement_deferred_callbacks = ordinary_callback_count == 0 && success_callback_count == 0;

    completion->RetireOrdinary(0);
    result.prefix_retirement_completed_callbacks =
        ordinary_callback_count == 1 && success_callback_count == 0;

    completion->RetireOrdinary(1);
    completion->RetireOrdinary(0);
    completion->RetireSuccess(0);
    result.repeated_retirement_suppressed = ordinary_callback_count == 1 && success_callback_count == 0;
    result.ordinary_callback_count        = ordinary_callback_count;
    result.success_callback_count         = success_callback_count;

    // Every materialized segment must retain the same source ownership payload.
    auto                                    source_owner    = std::make_shared<RHICommandLifetime>();
    std::weak_ptr<const RHICommandLifetime> source_lifetime = source_owner;
    CmdSubmit source_submit = MakeEmptySubmit();
    source_submit.lifetime                                  = std::move(source_owner);
    source_submit.cmds.emplace_back(
        MakeUnique<VulkanMaterializationProbeCommand>(
            EQueueType::Graphics
        )
    );
    source_submit.cmds.emplace_back(
        MakeUnique<VulkanMaterializationProbeCommand>(
            EQueueType::Compute
        )
    );
    source_submit.segments = {
        RHISubmitSegment{EQueueType::Graphics, 0, 1},
        RHISubmitSegment{EQueueType::Compute, 1, 2},
    };
    source_submit.resource_state_ownership =
        ERHIResourceStateOwnership::Compiled;
    source_submit.async_queue_scope = 0x50483139414B4545ull;

    FenceRef source_signal = RenderDevice::Get().CreateFence();
    if (!source_signal.IsValid()) {
        throw std::runtime_error(
            "multi-segment materialization probe failed to create source fence"
        );
    }
    auto* source_fence = ResourceCast(source_signal.Get());
    source_submit.signal_events.emplace_back(SignalEvent{
        .timeline_handle = uint64(source_signal.Get()),
        .value           = 7,
    });
    source_submit.signal_rejection_keepalives.emplace_back(
        source_signal
    );

    RHISubmissionBatch batch{};
    batch.sequence = 0x504831394D41544Cull;
    const RHISourceSubmitDescription description{
        .root_queue      = EQueueType::Graphics,
        .command_count   = source_submit.cmds.size(),
        .segments        = source_submit.segments,
        .execution_class = source_submit.translate_execution_class,
        .has_side_effects = true,
    };
    batch.topology = BuildRHISubmissionTopology(
        std::span<const RHISourceSubmitDescription>(
            &description, 1
        ),
        batch.sequence
    );
    if (!batch.topology.IsValid()) {
        throw std::runtime_error(
            "multi-segment materialization probe built an invalid topology"
        );
    }
    batch.submits.emplace_back(
        EQueueType::Graphics, std::move(source_submit)
    );

    const VulkanSegmentMaterializationResult materialization =
        MaterializeMultiSegmentBatch(
            batch, RenderDevice::Get().GetQueueTopology()
        );
    if (!materialization.changed || batch.submits.size() != 2) {
        throw std::runtime_error(
            "multi-segment materialization probe did not split its source"
        );
    }

    const CmdSubmit& prefix_submit = batch.submits[0].submit;
    const CmdSubmit& suffix_submit = batch.submits[1].submit;
    if (!prefix_submit.lifetime || prefix_submit.lifetime != suffix_submit.lifetime ||
        prefix_submit.lifetime != source_lifetime.lock()) {
        throw std::runtime_error("materialized segments did not share source resource ownership");
    }

    // An early-arriving packet must survive later participants and every tier.
    VulkanBatchCompletionGroup              group(2);
    auto                                    owner               = std::make_shared<RHICommandLifetime>();
    std::weak_ptr<const RHICommandLifetime> weak_owner          = owner;
    bool                                    callbacks_saw_owner = true;
    VulkanBatchCompletionGroup::Participant first;
    first.lifetime    = std::move(owner);
    first.gpu_success = true;
    first.callbacks.emplace_back([&] {
        callbacks_saw_owner &= !weak_owner.expired();
    });
    group.Arrive(0, std::move(first));
    if (weak_owner.expired()) {
        throw std::runtime_error("batch completion dropped an early participant's owner");
    }
    VulkanBatchCompletionGroup::Participant last;
    last.gpu_success = true;
    last.success_callbacks.emplace_back([&] {
        callbacks_saw_owner &= !weak_owner.expired();
    });
    group.Arrive(1, std::move(last));
    group.WaitUntilSettled();
    if (!callbacks_saw_owner || !weak_owner.expired()) {
        throw std::runtime_error("batch completion resource lifetime did not cover callback tiers");
    }
    result.materialized_prefix_signal_count =
        static_cast<uint32>(prefix_submit.signal_events.size());
    result.materialized_prefix_keepalive_count =
        static_cast<uint32>(
            prefix_submit.signal_rejection_keepalives.size()
        );
    result.materialized_suffix_signal_count =
        static_cast<uint32>(suffix_submit.signal_events.size());
    result.materialized_suffix_keepalive_count =
        static_cast<uint32>(
            suffix_submit.signal_rejection_keepalives.size()
        );
    result.materialized_signal_identity_matches =
        prefix_submit.signal_events.size() == 1 &&
        prefix_submit.signal_rejection_keepalives.size() == 1 &&
        prefix_submit.signal_events.front().timeline_handle ==
            uint64(
                prefix_submit.signal_rejection_keepalives.front().Get()
            ) &&
        suffix_submit.signal_events.size() == 2 &&
        suffix_submit.signal_rejection_keepalives.size() == 2 &&
        suffix_submit.signal_events.front().timeline_handle ==
            uint64(
                suffix_submit.signal_rejection_keepalives.front().Get()
            ) &&
        suffix_submit.signal_events.back().timeline_handle ==
            uint64(source_signal.Get()) &&
        suffix_submit.signal_rejection_keepalives.back().Get() ==
            source_signal.Get();
    result.source_signal_remained_unrejected =
        source_fence != nullptr && !source_fence->IsRejected(7) &&
        !source_fence->IsFailed();
    return result;
}

} // namespace Moer::Render
