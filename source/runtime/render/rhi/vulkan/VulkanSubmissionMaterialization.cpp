#include "VulkanSubmissionMaterialization.h"
#include "rhi/RHI.h"

#include <limits>
#include <stdexcept>

namespace Moer::Render::VulkanSubmissionDetail {
namespace {

struct VulkanOriginalSourceMaterialization {
    bool                                               multi_segment{false};
    size_t                                             executable_begin{0};
    size_t                                             executable_count{1};
    std::shared_ptr<VulkanMultiSegmentCompletionState> completion{};
};

} // namespace

VulkanSegmentMaterializationResult
MaterializeMultiSegmentBatch(RHIBackendSubmissionBatch& _batch, const RHIQueueTopology& _queue_topology) {
    VulkanSegmentMaterializationResult result{};
    result.original_source_count = _batch.submits.size();

    size_t executable_source_count = 0;
    for (const RHISourceSubmitPlan& source_plan : _batch.topology.source_plans) {
        result.changed              = result.changed || source_plan.segment_plan_count > 1;
        const size_t retained_count = source_plan.segment_plan_count > 1 ? source_plan.segment_plan_count : 1;
        if (retained_count > std::numeric_limits<size_t>::max() - executable_source_count) {
            throw std::runtime_error("multi-segment materialization source count overflow");
        }
        executable_source_count += retained_count;
    }

    if (executable_source_count > std::numeric_limits<uint32>::max()) {
        throw std::runtime_error("multi-segment materialization exceeds SubmissionKey capacity");
    }

    result.sources.reserve(executable_source_count);
    if (!result.changed) {
        for (size_t source_index = 0; source_index < _batch.submits.size(); ++source_index) {
            result.sources.emplace_back(VulkanExecutableSourceMetadata{
                .original_source_index = static_cast<uint32>(source_index),
                .source_segment_index  = 0,
                .source_segment_count  = 1,
            });
        }
        return result;
    }

    Array<VulkanOriginalSourceMaterialization> source_materializations(_batch.submits.size());
    Array<RHIBackendSubmissionBatchEntry>      executable_sources{};
    executable_sources.reserve(executable_source_count);

    for (size_t source_index = 0; source_index < _batch.submits.size(); ++source_index) {
        RHIBackendSubmissionBatchEntry& source_entry  = _batch.submits[source_index];
        CmdSubmit&                      source_submit = source_entry.submit;
        const RHISourceSubmitPlan&      source_plan   = _batch.topology.source_plans[source_index];

        VulkanOriginalSourceMaterialization& materialization = source_materializations[source_index];
        materialization.multi_segment                        = source_plan.segment_plan_count > 1;
        materialization.executable_begin                     = executable_sources.size();
        materialization.executable_count = materialization.multi_segment ? source_plan.segment_plan_count : 1;

        if (!materialization.multi_segment) {
            executable_sources.emplace_back(source_entry.queue, MakeEmptySubmit());
            result.sources.emplace_back(VulkanExecutableSourceMetadata{
                .original_source_index = static_cast<uint32>(source_index),
                .source_segment_index  = 0,
                .source_segment_count  = 1,
            });
            continue;
        }

        Array<FenceRef> segment_outcomes{};
        segment_outcomes.reserve(source_plan.segment_plan_count);
        for (size_t segment_index = 0; segment_index < source_plan.segment_plan_count; ++segment_index) {
            FenceRef outcome = RenderDevice::Get().CreateFence();
            if (!outcome.IsValid()) {
                throw std::runtime_error("multi-segment materialization failed to create completion fence");
            }
            segment_outcomes.emplace_back(std::move(outcome));
        }
        materialization.completion =
            std::make_shared<VulkanMultiSegmentCompletionState>(std::move(segment_outcomes));

        for (size_t segment_index = 0; segment_index < source_plan.segment_plan_count; ++segment_index) {
            const RHISubmissionSegmentPlan& segment_plan =
                _batch.topology.segments[source_plan.segment_plan_begin + segment_index];
            const RHISubmitSegment& descriptor    = segment_plan.segment;
            const size_t            command_count = descriptor.end - descriptor.begin;

            CmdSubmit segment_submit = MakeEmptySubmit();
            segment_submit.cmds.reserve(command_count);
            segment_submit.callbacks.reserve(1);
            segment_submit.success_callbacks.reserve(1);
            segment_submit.signal_events.reserve(
                1 + (segment_plan.inherit_source_signal_events_and_callbacks ?
                         source_submit.signal_events.size() :
                         0)
            );
            segment_submit.signal_rejection_keepalives.reserve(
                1 + (segment_plan.inherit_source_signal_events_and_callbacks ?
                         source_submit.signal_rejection_keepalives.size() :
                         0)
            );
            segment_submit.wait_events.reserve(
                (segment_plan.inherit_source_wait_events ? source_submit.wait_events.size() : 0) + 1
            );
            segment_submit.segments.reserve(1);

            segment_submit.resource_state_ownership  = source_submit.resource_state_ownership;
            segment_submit.translate_execution_class = source_submit.translate_execution_class;
            segment_submit.async_queue_scope         = source_submit.async_queue_scope;
            segment_submit.debug_label_color         = source_submit.debug_label_color;

            if (descriptor.queue != EQueueType::Copy) {
                segment_submit.cached_args = source_submit.cached_args;
            }
            if (segment_plan.inherit_source_runtime_payload) {
                segment_submit.debug_label = source_submit.debug_label;
            }

            segment_submit.segments.emplace_back(RHISubmitSegment{
                .queue = descriptor.queue,
                .begin = 0,
                .end   = command_count,
            });

            const FenceRef& segment_outcome = materialization.completion->SegmentOutcome(segment_index);
            segment_submit.signal_events.emplace_back(SignalEvent{
                .timeline_handle = uint64(segment_outcome.Get()),
                .value           = 1,
            });
            segment_submit.signal_rejection_keepalives.emplace_back(
                segment_outcome
            );
            segment_submit.callbacks.emplace_back([completion = materialization.completion, segment_index] {
                completion->RetireOrdinary(segment_index);
            });
            segment_submit.success_callbacks.emplace_back(
                [completion = materialization.completion, segment_index] {
                    completion->RetireSuccess(segment_index);
                }
            );

            bool cross_native_predecessor_wait = false;
            if (segment_index != 0) {
                const RHISubmissionSegmentPlan& predecessor_plan =
                    _batch.topology.segments[source_plan.segment_plan_begin + segment_index - 1];
                cross_native_predecessor_wait =
                    _queue_topology.Resolve(predecessor_plan.segment.queue).native_queue_id !=
                    _queue_topology.Resolve(descriptor.queue).native_queue_id;
                if (cross_native_predecessor_wait) {
                    const FenceRef& predecessor_outcome =
                        materialization.completion->SegmentOutcome(segment_index - 1);
                    segment_submit.wait_events.emplace_back(WaitEvent{
                        .timeline_handle = uint64(predecessor_outcome.Get()),
                        .value           = 1,
                    });
                    ++result.cross_native_wait_count;
                }
            }

            executable_sources.emplace_back(descriptor.queue, std::move(segment_submit));
            result.sources.emplace_back(VulkanExecutableSourceMetadata{
                .original_source_index         = static_cast<uint32>(source_index),
                .source_segment_index          = static_cast<uint32>(segment_index),
                .source_segment_count          = static_cast<uint32>(source_plan.segment_plan_count),
                .cross_native_predecessor_wait = cross_native_predecessor_wait,
            });
        }
    }

    assert(executable_sources.size() == executable_source_count);
    assert(result.sources.size() == executable_source_count);

    // Build and validate the flattened immutable topology before moving any
    // command, callback, or external synchronization ownership out of the
    // original sources. Every operation after this point is pre-reserved and
    // noexcept for the owned payload types.
    Array<RHISourceSubmitDescription> descriptions{};
    descriptions.reserve(executable_source_count);
    for (size_t source_index = 0; source_index < _batch.submits.size(); ++source_index) {
        const RHIBackendSubmissionBatchEntry&      source_entry    = _batch.submits[source_index];
        const VulkanOriginalSourceMaterialization& materialization = source_materializations[source_index];
        if (!materialization.multi_segment) {
            descriptions.emplace_back(RHISourceSubmitDescription{
                .root_queue      = source_entry.queue,
                .command_count   = source_entry.submit.cmds.size(),
                .segments        = source_entry.submit.segments,
                .execution_class = source_entry.submit.translate_execution_class,
                .has_side_effects =
                    SubmitHasSideEffects(source_entry.submit) || !source_entry.submit.cmds.empty(),
            });
            continue;
        }

        for (size_t segment_index = 0; segment_index < materialization.executable_count; ++segment_index) {
            const RHIBackendSubmissionBatchEntry& executable_entry =
                executable_sources[materialization.executable_begin + segment_index];
            descriptions.emplace_back(RHISourceSubmitDescription{
                .root_queue       = executable_entry.queue,
                .command_count    = executable_entry.submit.segments.front().end,
                .segments         = executable_entry.submit.segments,
                .execution_class  = executable_entry.submit.translate_execution_class,
                .has_side_effects = true,
            });
        }
    }

    RHISubmissionTopologyPlan executable_topology = BuildRHISubmissionTopology(descriptions, _batch.sequence);
    if (!executable_topology.IsValid() ||
        executable_topology.source_plans.size() != executable_sources.size()) {
        throw std::runtime_error("multi-segment materialization produced an invalid executable topology");
    }

    for (size_t source_index = 0; source_index < _batch.submits.size(); ++source_index) {
        RHIBackendSubmissionBatchEntry&      source_entry    = _batch.submits[source_index];
        VulkanOriginalSourceMaterialization& materialization = source_materializations[source_index];
        if (!materialization.multi_segment) {
            executable_sources[materialization.executable_begin].submit = std::move(source_entry.submit);
            continue;
        }

        materialization.completion->AdoptSourceCallbacks(
            std::move(source_entry.submit.callbacks), std::move(source_entry.submit.success_callbacks)
        );

        const RHISourceSubmitPlan& source_plan = _batch.topology.source_plans[source_index];
        for (size_t segment_index = 0; segment_index < materialization.executable_count; ++segment_index) {
            const RHISubmitSegment& descriptor =
                _batch.topology.segments[source_plan.segment_plan_begin + segment_index].segment;
            CmdSubmit& executable_submit =
                executable_sources[materialization.executable_begin + segment_index].submit;
            for (size_t command_index = descriptor.begin; command_index < descriptor.end; ++command_index) {
                executable_submit.cmds.emplace_back(std::move(source_entry.submit.cmds[command_index]));
            }
        }

        CmdSubmit& first_submit = executable_sources[materialization.executable_begin].submit;
        for (const WaitEvent& wait : source_entry.submit.wait_events) {
            first_submit.wait_events.emplace_back(wait);
        }

        CmdSubmit& last_submit =
            executable_sources[materialization.executable_begin + materialization.executable_count - 1]
                .submit;
        last_submit.gpu_completion_tokens =
            std::move(source_entry.submit.gpu_completion_tokens);
        last_submit.gpu_completion_publish_batch =
            source_entry.submit.gpu_completion_publish_batch;
        source_entry.submit.gpu_completion_publish_batch = {};
        for (const SignalEvent& signal : source_entry.submit.signal_events) {
            last_submit.signal_events.emplace_back(signal);
        }
        for (FenceRef& keepalive :
             source_entry.submit.signal_rejection_keepalives) {
            last_submit.signal_rejection_keepalives.emplace_back(
                std::move(keepalive)
            );
        }
        // Signal ownership has moved to the last executable segment. Leaving
        // the original handles live would make CmdSubmit::~CmdSubmit reject
        // values that have already been accepted by the native queue.
        source_entry.submit.signal_events.clear();
        source_entry.submit.signal_rejection_keepalives.clear();
    }

    _batch.submits  = std::move(executable_sources);
    _batch.topology = std::move(executable_topology);
    return result;
}

} // namespace Moer::Render::VulkanSubmissionDetail
