#include "VulkanSubmissionExecutor.h"
#include "VulkanSubmissionMaterialization.h"
#include "VulkanTimeline.h"

#include "VulkanCommandRecorder.h"
#include "VulkanDevice.h"
#include "VulkanQueue.h"
#include "log/LogSystem.h"
#include "misc/RAII.h"
#include "platform/Platform.h"
#include "profile/ProfileScope.h"
#include "rhi/RHI.h"
#include "rhi/RHISubmissionPipelinePolicy.h"
#include "rhi/RHIThreadOwnership.h"
#include "taskgraph/TaskGraph.h"
#if MOER_VULKAN_TEST_SUPPORT
#include "vulkan/VulkanSubmissionTestHooksInternal.h"
#endif
#include "VulkanTranslateWaveScheduler.h"

#include <algorithm>
#include <cassert>
#include <latch>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>

namespace Moer::Render {
using namespace VulkanSubmissionDetail;
namespace {

bool SameNativeQueue(const RHIQueueTopology& _topology, EQueueType _lhs, EQueueType _rhs) {
    return _topology.Resolve(_lhs).native_queue_id == _topology.Resolve(_rhs).native_queue_id;
}

bool SubmitContainsQuery(const CmdSubmit& _submit) {
    return !_submit.query_tokens.empty() ||
           std::any_of(
               _submit.cmds.begin(),
               _submit.cmds.end(),
               [](const UniquePtr<Command>& _command) {
                   return _command &&
                          _command->Type() == Command::EType::Query;
               }
           );
}

bool IsNativeQueue(EQueueType _queue) {
    return _queue == EQueueType::Graphics || _queue == EQueueType::Compute ||
           _queue == EQueueType::Copy;
}

bool IsCopyCommand(Command::EType _type) {
    switch (_type) {
        case Command::EType::UploadBuffer:
        case Command::EType::CopyBackBuffer:
        case Command::EType::BufferToBuffer:
        case Command::EType::BufferToTexture:
        case Command::EType::TextureToBuffer:
        case Command::EType::UploadTexture:
        case Command::EType::TextureToTexture:
        case Command::EType::CopyBackTexture:
        case Command::EType::Barrier:
        case Command::EType::QueueTransfer:
        // Scope commands are queue-agnostic debug-label boundaries. Modern
        // GPU scopes lower to Scope + Query pairs; VulkanCopyQueue already routes
        // both through VkCmdVisitor, so rejecting the label half
        // would reject an otherwise legal Copy timestamp source.
        case Command::EType::Scope:
        case Command::EType::ResourceAccessScope:
        // Timestamp Query commands are legal ordering boundaries inside one
        // Copy segment. Multi-segment sources still reject all queries above,
        // and VulkanCopyQueue performs token/queue/LIFO preflight before native
        // command-buffer recording.
        case Command::EType::Query:
            return true;
        default:
            return false;
    }
}

bool IsParallelTranslateCandidate(const RHISubmissionRequest& _entry) {
    return IsNativeQueue(_entry.queue) &&
           _entry.submit.translate_execution_class == ERHITranslateExecutionClass::Parallel &&
           _entry.submit.HasExplicitResourceStateOwnership() && _entry.submit.async_queue_scope != 0;
}

struct BatchPreflightResult {
    bool             valid{true};
    std::string_view reason{"none"};
    size_t           source_index{std::numeric_limits<size_t>::max()};
    size_t           command_index{std::numeric_limits<size_t>::max()};
};

BatchPreflightResult RejectPreflight(
    std::string_view _reason,
    size_t           _source_index  = std::numeric_limits<size_t>::max(),
    size_t           _command_index = std::numeric_limits<size_t>::max()
) {
    return {
        .valid         = false,
        .reason        = _reason,
        .source_index  = _source_index,
        .command_index = _command_index,
    };
}

BatchPreflightResult ValidateBatch(const RHISubmissionBatch& _batch) {
    const RHISubmissionTopologyPlan& topology = _batch.topology;
    if (!topology.IsValid()) {
        return RejectPreflight("invalid topology", topology.error_source_index);
    }
    if (topology.source_plans.size() != _batch.submits.size()) {
        return RejectPreflight("source/topology count mismatch");
    }
    if (_batch.present && (!_batch.present->swapchain || _batch.present->source.texture == nullptr)) {
        return RejectPreflight("invalid Present request");
    }

    size_t expected_segment_begin = 0;
    for (size_t source_index = 0; source_index < _batch.submits.size(); ++source_index) {
        const RHISubmissionRequest& entry           = _batch.submits[source_index];
        const CmdSubmit&            submit          = entry.submit;
        const RHISourceSubmitPlan&  source_plan     = topology.source_plans[source_index];
        const bool                  submit_has_work = !submit.cmds.empty() || SubmitHasSideEffects(submit);

        if (!IsNativeQueue(entry.queue)) {
            return RejectPreflight("invalid source queue", source_index);
        }
        if (source_plan.source_key.op_seq != _batch.sequence ||
            source_plan.source_key.submit_idx != source_index || source_plan.root_queue != entry.queue ||
            source_plan.command_count != submit.cmds.size() ||
            source_plan.execution_class != submit.translate_execution_class ||
            source_plan.has_side_effects != submit_has_work) {
            return RejectPreflight("source plan does not match payload", source_index);
        }
        if (source_plan.segment_plan_begin != expected_segment_begin) {
            return RejectPreflight("non-contiguous source plans", source_index);
        }

        const RHISubmitSegment synthesized_segment{
            .queue = source_plan.root_queue,
            .begin = 0,
            .end   = submit.cmds.size(),
        };
        const std::span<const RHISubmitSegment> source_segments =
            submit.segments.empty() ? std::span<const RHISubmitSegment>(&synthesized_segment, 1) :
                                      std::span<const RHISubmitSegment>(submit.segments);
        const size_t expected_segment_count = submit_has_work ? source_segments.size() : 0;
        if (source_plan.segment_plan_count != expected_segment_count) {
            return RejectPreflight("source retention does not match payload", source_index);
        }

        const bool multi_segment = expected_segment_count > 1;
        if (multi_segment && (!submit.HasExplicitResourceStateOwnership() || submit.async_queue_scope == 0)) {
            return RejectPreflight(
                "multi-segment source requires explicit state and async scope", source_index
            );
        }
        if (multi_segment && SubmitContainsQuery(submit)) {
            // A Phase 17A timestamp pair is allocator-local and must not cross
            // a materialized native command-buffer/queue segment boundary.
            return RejectPreflight(
                "multi-segment source contains timestamp query", source_index
            );
        }
        if (multi_segment && (submit.b_sync || submit.b_delete_resources ||
                              submit.ProfilingPhase() != ERHIProfilingPhase::Disabled)) {
            return RejectPreflight("multi-segment source contains unsupported control payload", source_index);
        }

        std::optional<size_t> runtime_payload_segment{};
        for (size_t segment_index = expected_segment_count; segment_index > 0; --segment_index) {
            if (source_segments[segment_index - 1].queue != EQueueType::Copy) {
                runtime_payload_segment = segment_index - 1;
                break;
            }
        }
        if (!runtime_payload_segment &&
            (!submit.cached_args.empty() || !submit.debug_label.empty() || submit.b_sync ||
             submit.b_delete_resources || submit.ProfilingPhase() != ERHIProfilingPhase::Disabled)) {
            return RejectPreflight("Copy-only source contains unsupported runtime payload", source_index);
        }

        for (size_t segment_index = 0; segment_index < expected_segment_count; ++segment_index) {
            const size_t flat_segment_index = expected_segment_begin + segment_index;
            if (flat_segment_index >= topology.segments.size()) {
                return RejectPreflight("source segment is out of range", source_index);
            }
            const RHISubmitSegment&         descriptor   = source_segments[segment_index];
            const RHISubmissionSegmentPlan& segment_plan = topology.segments[flat_segment_index];
            if (segment_plan.key.op_seq != _batch.sequence ||
                segment_plan.key.submit_idx != flat_segment_index ||
                segment_plan.source_key != source_plan.source_key ||
                segment_plan.source_segment_index != segment_index ||
                segment_plan.segment.queue != descriptor.queue ||
                segment_plan.segment.begin != descriptor.begin ||
                segment_plan.segment.end != descriptor.end ||
                segment_plan.execution_class != source_plan.execution_class ||
                segment_plan.inherit_source_wait_events != (segment_index == 0) ||
                segment_plan.inherit_source_signal_events_and_callbacks !=
                    (segment_index + 1 == expected_segment_count) ||
                segment_plan.inherit_source_runtime_payload !=
                    (runtime_payload_segment.has_value() && *runtime_payload_segment == segment_index)) {
                return RejectPreflight("segment plan does not match source", source_index);
            }
            for (const SubmissionKey& dependency : segment_plan.dependencies) {
                if (dependency.op_seq != _batch.sequence || dependency.submit_idx >= flat_segment_index) {
                    return RejectPreflight(
                        "source segment has an unknown or non-preceding dependency", source_index
                    );
                }
            }

            if (expected_segment_count == 1 &&
                (descriptor.queue != source_plan.root_queue || descriptor.begin != 0 ||
                 descriptor.end != submit.cmds.size())) {
                return RejectPreflight("single segment does not match root queue", source_index);
            }

            for (size_t command_index = descriptor.begin; command_index < descriptor.end; ++command_index) {
                const UniquePtr<Command>& command = submit.cmds[command_index];
                if (!command) {
                    return RejectPreflight("null command", source_index, command_index);
                }
                if (command->Type() >= Command::EType::Count) {
                    return RejectPreflight("invalid command type", source_index, command_index);
                }
                if (descriptor.queue == EQueueType::Copy && !IsCopyCommand(command->Type())) {
                    return RejectPreflight(
                        "unsupported command on Copy segment", source_index, command_index
                    );
                }
            }
        }

        for (const WaitEvent& wait : submit.wait_events) {
            if (wait.timeline_handle == 0) {
                return RejectPreflight("null wait fence", source_index);
            }
        }
        for (const SignalEvent& signal : submit.signal_events) {
            if (signal.timeline_handle == 0) {
                return RejectPreflight("null signal fence", source_index);
            }
        }

        if (expected_segment_count == 1 && source_segments.front().queue == EQueueType::Copy) {
            if (!submit.cached_args.empty()) {
                return RejectPreflight("Copy submit contains cached arguments", source_index);
            }
            if (submit.b_sync || submit.b_tick_profiling || submit.b_delete_resources) {
                return RejectPreflight("Copy submit contains unsupported control flags", source_index);
            }
            if (!submit.debug_label.empty()) {
                return RejectPreflight("Copy submit debug labels are not supported", source_index);
            }
        }

        expected_segment_begin += source_plan.segment_plan_count;
    }

    if (expected_segment_begin != topology.segments.size()) {
        return RejectPreflight("unclaimed topology segments");
    }
    return {};
}

const char* TopologyErrorName(ERHISubmissionTopologyError _error) {
    switch (_error) {
        case ERHISubmissionTopologyError::None:
            return "none";
        case ERHISubmissionTopologyError::InvalidRootQueue:
            return "invalid-root-queue";
        case ERHISubmissionTopologyError::InvalidExecutionClass:
            return "invalid-execution-class";
        case ERHISubmissionTopologyError::InvalidSegmentQueue:
            return "invalid-segment-queue";
        case ERHISubmissionTopologyError::InvalidSegmentRange:
            return "invalid-segment-range";
        case ERHISubmissionTopologyError::NonContiguousSegmentCoverage:
            return "non-contiguous-segment-coverage";
        case ERHISubmissionTopologyError::IndexOverflow:
            return "index-overflow";
    }
    return "unknown";
}

void ResolveRejectedPresent(std::optional<RHIPresentRequest>& _present) {
    if (_present && _present->receipt) {
        _present->receipt->Resolve(false, EPresentStatus::Rejected, EPresentStage::Validation);
    }
}

} // namespace

VulkanSubmissionExecutor::VulkanSubmissionExecutor(uint32 _batch_window) :
    batch_window(
        RHISubmissionPipelinePolicy::ClampBatchWindow(_batch_window)
    ) {
    const size_t           configured_batch_window = batch_window;
    const RHIQueueTopology queue_topology =
        RenderDevice::Get().GetQueueTopology();
    runtime_queues_share_native_lane =
        RHISubmissionPipelinePolicy::HasAvailableNativeLaneAlias(
            queue_topology
        );
    // Logical queue wrappers own independent transferable gates. Keep one
    // batch in flight whenever any available Graphics/Compute/Copy pair
    // aliases so those gates cannot admit two packets for one native lane.
    batch_window =
        RHISubmissionPipelinePolicy::ResolveEffectiveBatchWindow(
            _batch_window, queue_topology
        );

    auto& graphics_queue = static_cast<VulkanCommandQueue&>(
        RenderDevice::Get().GetCommandQueue(EQueueType::Graphics)
    );
    auto& compute_queue = static_cast<VulkanCommandQueue&>(
        RenderDevice::Get().GetCommandQueue(EQueueType::Compute)
    );
    auto& copy_queue =
        static_cast<VulkanCopyQueue&>(RenderDevice::Get().GetCopyQueue());
    bool graphics_claimed = false;
    bool compute_claimed  = false;
    bool copy_claimed     = false;
    try {
        graphics_claimed = graphics_queue.ClaimRuntimeOwnership();
        if (!graphics_claimed) {
            throw std::runtime_error(
                "Vulkan Graphics queue is already claimed by an executor"
            );
        }
        compute_claimed = compute_queue.ClaimRuntimeOwnership();
        if (!compute_claimed) {
            throw std::runtime_error(
                "Vulkan Compute queue is already claimed by an executor"
            );
        }
        copy_claimed = copy_queue.ClaimRuntimeOwnership();
        if (!copy_claimed) {
            throw std::runtime_error(
                "Vulkan Copy queue is already claimed by an executor"
            );
        }
        claims_owned = true;

        submission_thread = std::jthread([this] {
            RunSubmissionThread();
        });
        translate_thread  = std::jthread([this] {
            RunTranslateThread();
        });
        LOG_INFO(
            "[RHIExecutor][Vulkan] ownership runtime started "
            "Executor=1 TranslateCoordinator=1 TranslateWorkers=TaskGraph "
            "Submission=1 configured_batch_window={} batch_window={} "
            "runtime_native_alias={}",
            configured_batch_window,
            batch_window,
            runtime_queues_share_native_lane
        );
    } catch (...) {
        graphics_queue.CancelRuntimeDependencyWaits();
        compute_queue.CancelRuntimeDependencyWaits();
        copy_queue.CancelRuntimeDependencyWaits();
        if (translate_thread.joinable()) {
            {
                std::lock_guard lock(mutex);
                accepting         = false;
                constructor_abort = true;
            }
            cv.notify_all();
            translate_thread.join();
        }
        if (submission_thread.joinable()) {
            StopSubmissionThread();
        }
        if (copy_claimed) {
            copy_queue.ReleaseRuntimeOwnership();
        }
        if (compute_claimed) {
            compute_queue.ReleaseRuntimeOwnership();
        }
        if (graphics_claimed) {
            graphics_queue.ReleaseRuntimeOwnership();
        }
        claims_owned = false;
        throw;
    }
}

VulkanSubmissionExecutor::~VulkanSubmissionExecutor() {
    ShutDown();
    if (claims_owned) {
        auto& graphics_queue = static_cast<VulkanCommandQueue&>(
            RenderDevice::Get().GetCommandQueue(EQueueType::Graphics)
        );
        auto& compute_queue = static_cast<VulkanCommandQueue&>(
            RenderDevice::Get().GetCommandQueue(EQueueType::Compute)
        );
        auto& copy_queue =
            static_cast<VulkanCopyQueue&>(RenderDevice::Get().GetCopyQueue());
        copy_queue.ReleaseRuntimeOwnership();
        compute_queue.ReleaseRuntimeOwnership();
        graphics_queue.ReleaseRuntimeOwnership();
        claims_owned = false;
    }
}

VulkanSubmissionExecutor::BatchExecution::BatchExecution(RHISubmissionBatch& _batch, bool _pipeline) :
    sequence(_batch.sequence) {
    if (_pipeline) {
        submission_tasks_done.emplace();
    }
    sources.reserve(_batch.submits.size());
    const GpuCompletionPublishBatch common_completion_batch =
        GpuCompletionBackendAccess::BeginPublishBatch();
    const QueryPublishBatch common_query_batch =
        QueryBackendAccess::BeginPublishBatch();

    for (size_t source_index = 0;
         source_index < _batch.submits.size();
         ++source_index) {
        RHISubmissionRequest& entry = _batch.submits[source_index];
        SubmissionSource      source{};
        source.source_index      = source_index;
        source.queue_type        = entry.queue;
        source.async_queue_scope = entry.submit.async_queue_scope;
        const auto& plan         = _batch.topology.source_plans[source_index];
        if (plan.segment_plan_count != 0) {
            source.key = _batch.topology.segments[plan.segment_plan_begin].key;
        }
        source.signals.reserve(entry.submit.signal_events.size());
        for (const SignalEvent& signal : entry.submit.signal_events) {
            auto* fence = reinterpret_cast<Fence*>(signal.timeline_handle);
            if (fence != nullptr) {
                source.signals.emplace_back(FenceRef(fence), signal.value);
            }
        }

        source.gpu_completion_tokens =
            entry.submit.gpu_completion_tokens;
        if (!source.gpu_completion_tokens.empty()) {
            source.gpu_completion_batch =
                entry.submit.gpu_completion_publish_batch.Valid() ?
                    entry.submit.gpu_completion_publish_batch :
                    common_completion_batch;
            entry.submit.gpu_completion_publish_batch =
                source.gpu_completion_batch;
        }
        source.query_tokens = entry.submit.query_tokens;
        if (!source.query_tokens.empty()) {
            source.query_batch =
                entry.submit.query_publish_batch.Valid() ?
                    entry.submit.query_publish_batch :
                    common_query_batch;
            // Install the notification ticket before the CmdSubmit can move
            // into either a recorded packet or a per-queue Completion packet.
            entry.submit.query_publish_batch = source.query_batch;
        }
        sources.emplace_back(std::move(source));
    }
}

void VulkanSubmissionExecutor::BatchExecution::PublishSuffixRejection(
    size_t           _reject_from,
    int32            _result,
    bool             _recoverable,
    std::string_view _reason
) noexcept {
    const size_t   begin = std::min(_reject_from, sources.size());
    const VkResult result =
        _result == static_cast<int32>(VK_SUCCESS) ?
            VK_ERROR_UNKNOWN :
            static_cast<VkResult>(_result);

    const std::string_view reason =
        _reason.empty() ?
            "Vulkan submission suffix was rejected before GPU completion" :
            _reason;

    uint64          publication_epoch = 0;
    std::lock_guard lock(publication_mutex);
    publication_epoch = next_publication_epoch++;
    if (publication_epoch == 0) {
        publication_epoch = next_publication_epoch++;
    }
    for (size_t source_index = begin;
         source_index < sources.size();
         ++source_index) {
        SubmissionSource& source = sources[source_index];
        if (source.publication_epoch != 0) {
            continue;
        }
        if (source.gpu_completion_batch.Valid()) {
            GpuCompletionBackendAccess::PublishErrorsIfPending(
                source.gpu_completion_tokens,
                reason,
                source.gpu_completion_batch
            );
        }
        if (source.query_batch.Valid()) {
            QueryBackendAccess::PublishErrorsIfPending(
                source.query_tokens, reason, source.query_batch
            );
        }
        source.publication_epoch = publication_epoch;
    }

    // All matching host observations are terminal before external Fence code.
    // Fence::Reject/Fail may synchronously re-enter this publication object;
    // the recursive mutex lets that call observe the already-claimed sources
    // while keeping concurrent publishers behind the complete signal phase.
    // The epoch prevents the nested call from replaying any Fence operation.
    for (size_t source_index = begin;
         source_index < sources.size();
         ++source_index) {
        SubmissionSource& source = sources[source_index];
        if (source.publication_epoch != publication_epoch) {
            continue;
        }
        for (const auto& [fence_ref, value] : source.signals) {
            auto* fence =
                reinterpret_cast<VulkanTimeline*>(fence_ref.Get());
            if (fence == nullptr) {
                continue;
            }
            if (_recoverable) {
                fence->Reject(value);
                continue;
            }
            try {
                fence->Fail(result);
            } catch (...) {
                // Preserve terminal progress even if platform synchronization
                // bookkeeping itself cannot publish the richer hard-fault
                // classification.
                fence->Reject(value);
            }
        }
    }
}

void VulkanSubmissionExecutor::BatchExecution::AddWork() noexcept {
    work_state.AddWork();
}

void VulkanSubmissionExecutor::BatchExecution::FinishWork() noexcept {
    if (work_state.FinishWork()) {
        submission_tasks_done->set_value();
    }
}

void VulkanSubmissionExecutor::BatchExecution::Seal() noexcept {
    if (work_state.Seal()) {
        submission_tasks_done->set_value();
    }
}

void VulkanSubmissionExecutor::BatchExecution::PublishFailure(size_t first, int32 result, bool recoverable) noexcept {
    {
        std::lock_guard lock(failure_mutex);
        if (first >= reject_from) {
            return;
        }
        reject_from = first;
        failure     = {
                .outcome               = {EVulkanOperationStatus::Rejected, result == VK_SUCCESS ? VK_ERROR_UNKNOWN : static_cast<VkResult>(result)},
                .recoverable_rejection = recoverable,
        };
    }
    PublishSuffixRejection(first, result, recoverable, recoverable ? "Vulkan submission pipeline suffix was rejected by a dependency" : "Vulkan submission pipeline suffix was rejected after a hard failure");
}

std::optional<VulkanSubmissionResult> VulkanSubmissionExecutor::BatchExecution::ReadFailure(size_t source_index) const noexcept {
    std::lock_guard lock(failure_mutex);
    return source_index >= reject_from ? std::optional{failure} : std::nullopt;
}

void VulkanSubmissionExecutor::Enqueue(RHISubmissionBatch&& _batch) {
    static_assert(
        std::is_nothrow_move_assignable_v<RHISubmissionBatch>,
        "backend FIFO publication requires a nonthrowing batch handoff"
    );
    bool rejected = false;
    {
        std::lock_guard lock(mutex);
        if (!accepting) {
            rejected = true;
        } else {
            // Allocate the deque slot before moving the batch. If allocation
            // throws, RHIExecutor still owns an intact batch and can reject
            // every signal/receipt. The subsequent assignment is statically
            // required to be nonthrowing.
            requests.emplace_back();
            WorkerRequest& request = requests.back();
            request.kind           = ERequestKind::Submit;
            request.batch          = std::move(_batch);
        }
    }
    if (rejected) {
        // User completion callbacks must never run while the backend FIFO lock
        // is held: they may enqueue more RHI work or acquire unrelated locks.
        RejectBatch(
            std::move(_batch),
            static_cast<int32>(VK_ERROR_UNKNOWN),
            "submission runtime is stopping or stopped",
            true
        );
        return;
    }
    cv.notify_one();
}

void VulkanSubmissionExecutor::Sync(ERHISyncDepth _depth) {
    std::promise<void> reply;
    auto               completion = reply.get_future();
    {
        std::lock_guard lock(mutex);
        if (!accepting) {
            throw std::runtime_error("Sync rejected during shutdown");
        }
        requests.emplace_back(
            WorkerRequest{
                .kind       = ERequestKind::Sync,
                .sync_depth = _depth,
                .reply      = std::move(reply),
            }
        );
    }
    cv.notify_one();
#if MOER_VULKAN_TEST_SUPPORT
    NotifyVulkanBackendSyncWait();
#endif
    completion.get();
}

void VulkanSubmissionExecutor::Flush() {
    // Enqueue publishes directly to the runtime FIFO and wakes its owner.
    // Flush does not wait; Sync is the explicit host boundary.
    cv.notify_one();
}

void VulkanSubmissionExecutor::BeginShutdown() noexcept {
    try {
        auto& graphics_queue = static_cast<VulkanCommandQueue&>(
            RenderDevice::Get().GetCommandQueue(EQueueType::Graphics)
        );
        auto& compute_queue = static_cast<VulkanCommandQueue&>(
            RenderDevice::Get().GetCommandQueue(EQueueType::Compute)
        );
        auto& copy_queue =
            static_cast<VulkanCopyQueue&>(RenderDevice::Get().GetCopyQueue());

        // A Submission owner can be blocked waiting for a producer timeline
        // which shutdown has stopped admitting. Publish cancellation without
        // waiting for the backend FIFO so concurrent Sync calls can drain.
        graphics_queue.CancelRuntimeDependencyWaits();
        compute_queue.CancelRuntimeDependencyWaits();
        copy_queue.CancelRuntimeDependencyWaits();
    } catch (...) {
        // RenderDevice queue access is expected to remain valid until backend
        // destruction. Keep this hook noexcept so lifecycle cleanup can still
        // advance if that contract is broken by an exceptional teardown.
    }
}

void VulkanSubmissionExecutor::ShutDown() {
    BeginShutdown();
    bool                     join_owner = false;
    std::shared_future<void> completion;
    {
        std::lock_guard lock(mutex);
        if (stopped) {
            return;
        }
        completion = stop_completion;
    }
    // Allocate the reply before changing lifecycle state; failure remains retryable.
    std::optional<std::promise<void>> reply;
    std::shared_future<void>          candidate;
    if (!completion.valid()) {
        reply.emplace();
        candidate = reply->get_future().share();
    }
    {
        std::lock_guard lock(mutex);
        if (stopped) {
            return;
        }
        if (!stop_completion.valid()) {
            requests.emplace_back(WorkerRequest{
                .kind       = ERequestKind::Stop,
                .sync_depth = ERHISyncDepth::Present,
                .reply      = std::move(reply),
            });
            stop_completion = std::move(candidate);
            join_owner      = true;
        }
        accepting  = false;
        completion = stop_completion;
    }
    cv.notify_one();
    completion.get();
    if (join_owner) {
        if (translate_thread.joinable()) {
            translate_thread.join();
        }
        {
            std::lock_guard lock(mutex);
            stopped = true;
        }
        cv.notify_all();
    } else {
        std::unique_lock lock(mutex);
        cv.wait(lock, [this] {
            return stopped;
        });
    }
}

void VulkanSubmissionExecutor::RunTranslateThread() {
    Platform::SetCurrentThreadName("Moer Vulkan Translate");
    RHIThreadRoleScope owner_scope(ERHIThreadRole::Translate);
    while (true) {
        WorkerRequest request{};
        {
            std::unique_lock lock(mutex);
            cv.wait(lock, [this] {
                return constructor_abort || !requests.empty();
            });
            if (constructor_abort && requests.empty()) {
                break;
            }
            request = std::move(requests.front());
            requests.pop_front();
        }

        MOER_PROFILE_SCOPE("Vulkan.ProcessWorkerRequest");
        size_t                                                    first_unconsumed_source = 0;
        std::shared_ptr<BatchExecution>                           execution;
        const VulkanSubmissionDetail::WorkerRequestDispatchResult dispatch_result =
            VulkanSubmissionDetail::DispatchWorkerRequestNoexcept(
                request.kind,
                [&] {
                    if (request.kind == ERequestKind::Submit) {
                        ProcessBatch(request.batch, first_unconsumed_source, execution);
                    } else {
                        DrainPipelineBatches();
                        ProcessSync(request.sync_depth);
                    }
                },
                [&] {
                    // ProcessBatch may throw before it reaches the ordinary
                    // pipeline admission/drain decision. Preserve the
                    // already-admitted stream prefix before publishing any
                    // rejection markers for this request.
                    DrainPipelineBatches();
                    LatchHardFailure(
                        SubmissionPosition{
                            request.batch.sequence,
                            first_unconsumed_source,
                        },
                        static_cast<int32>(VK_ERROR_UNKNOWN)
                    );
                    RejectBatch(
                        std::move(request.batch),
                        static_cast<int32>(VK_ERROR_UNKNOWN),
                        "unhandled submission request exception",
                        false,
                        first_unconsumed_source,
                        &first_unconsumed_source
                    );
                },
                [&](const std::exception_ptr& _failure) {
                    // Stop must still release its waiter so ShutDown can join
                    // the owners. Sync reports failure for every requested depth.
                    CompleteRequest(
                        request.reply,
                        request.kind == ERequestKind::Sync ?
                            _failure :
                            std::exception_ptr{}
                    );
                },
                [&](VulkanSubmissionDetail::EWorkerRequestFailurePhase _phase,
                    const std::exception_ptr&                          _exception) {
                    LatchHardFailure(
                        SubmissionPosition{
                            request.batch.sequence,
                            first_unconsumed_source,
                        },
                        static_cast<int32>(VK_ERROR_UNKNOWN)
                    );
                    ReportRequestFailure(request.kind, _phase, _exception);
                }
            );
        if (dispatch_result.stop) {
            break;
        }
    }
    StopSubmissionThread();
}

bool VulkanSubmissionExecutor::EnqueueSubmissionWork(SubmissionWork&& _work) {
    {
        std::lock_guard lock(submission_mutex);
        if (!submission_accepting) {
            return false;
        }
        submission_work.emplace_back(std::move(_work));
    }
    submission_cv.notify_one();
    return true;
}

void VulkanSubmissionExecutor::RunSubmissionThread() {
    Platform::SetCurrentThreadName("Moer Vulkan Submission");
    RHIThreadRoleScope owner_scope(ERHIThreadRole::Submission);
    while (true) {
        SubmissionWork work{};
        {
            std::unique_lock lock(submission_mutex);
            submission_cv.wait(lock, [this] {
                return !submission_accepting || !submission_work.empty();
            });
            if (submission_work.empty()) {
                assert(!submission_accepting);
                break;
            }
            work = std::move(submission_work.front());
            submission_work.pop_front();
        }
        if (work.valid()) {
            MOER_PROFILE_SCOPE("Vulkan.ExecuteSubmissionWork");
            // packaged_task captures exceptions and publishes them to the
            // waiting Translate owner; none may escape this service thread.
            work();
        }
    }
}

void VulkanSubmissionExecutor::StopSubmissionThread() {
    {
        std::lock_guard lock(submission_mutex);
        submission_accepting = false;
    }
    submission_cv.notify_all();
    if (submission_thread.joinable()) {
        submission_thread.join();
    }
}

void VulkanSubmissionExecutor::WaitForPipelineCapacity() {
    while (pending_submission_batches.size() >= batch_window) {
        MOER_PROFILE_SCOPE("Vulkan.WaitForPipelineCapacity");
        auto completion = std::move(pending_submission_batches.front());
        pending_submission_batches.pop_front();
        completion.get();
    }
}

void VulkanSubmissionExecutor::DrainPipelineBatches() {
    assert(
        GetCurrentRHIThreadRole() != ERHIThreadRole::Submission &&
        "Submission owner must not wait on its own pipeline work"
    );
    while (!pending_submission_batches.empty()) {
        auto completion = std::move(pending_submission_batches.front());
        pending_submission_batches.pop_front();
        completion.get();
    }
}

void VulkanSubmissionExecutor::ResetStreamScope() noexcept {
    active_async_queue_scope = 0;
    async_scope_entry_frontier.fill(std::nullopt);
    async_scope_seen_queues.fill(false);
}

void VulkanSubmissionExecutor::AppendFrontierWaits(
    CmdSubmit&           _submit,
    EQueueType           _queue,
    const QueueFrontier& _frontier
) const {
    const auto topology = RenderDevice::Get().GetQueueTopology();
    for (size_t index = 0; index < _frontier.size(); ++index) {
        if (!_frontier[index] || SameNativeQueue(topology, static_cast<EQueueType>(index), _queue)) {
            continue;
        }
        const WaitEvent event  = *_frontier[index];
        const bool      exists = std::any_of(
            _submit.wait_events.begin(), _submit.wait_events.end(), [&](const WaitEvent& candidate) {
                return candidate.timeline_handle == event.timeline_handle && candidate.value == event.value;
            }
        );
        if (!exists) {
            _submit.wait_events.emplace_back(event);
        }
    }
}

bool VulkanSubmissionExecutor::HasForeignAliasedFrontier(EQueueType _queue) const {
    const auto topology = RenderDevice::Get().GetQueueTopology();
    for (size_t index = 0; index < gpu_frontier.size(); ++index) {
        const auto source_queue = static_cast<EQueueType>(index);
        if (gpu_frontier[index] && source_queue != _queue && SameNativeQueue(topology, source_queue, _queue)) {
            return true;
        }
    }
    return false;
}

void VulkanSubmissionExecutor::PrepareStreamSubmit(
    CmdSubmit& _submit,
    EQueueType _queue
) {
    const RHIQueueTopology queue_topology =
        RenderDevice::Get().GetQueueTopology();
    auto mark_scope_queue_seen = [&] {
        for (size_t index = 0; index < async_scope_seen_queues.size();
             ++index) {
            if (SameNativeQueue(queue_topology, static_cast<EQueueType>(index), _queue)) {
                async_scope_seen_queues[index] = true;
            }
        }
    };

    const uint64 async_scope = _submit.async_queue_scope;
    if (async_scope == 0) {
        ResetStreamScope();
        AppendFrontierWaits(_submit, _queue, gpu_frontier);
        return;
    }

    if (active_async_queue_scope != async_scope) {
        active_async_queue_scope   = async_scope;
        async_scope_entry_frontier = gpu_frontier;
        async_scope_seen_queues.fill(false);
        if (!logged_async_queue_scope) {
            LOG_INFO(
                "[RHIExecutor][Vulkan][AsyncQueueScope] "
                "mode=dependency-led native_submit_owner=serial "
                "legacy_boundary=frontier"
            );
            logged_async_queue_scope = true;
        }
    }

    const size_t queue_index = static_cast<size_t>(_queue);
    if (!async_scope_seen_queues[queue_index]) {
        AppendFrontierWaits(_submit, _queue, async_scope_entry_frontier);
        mark_scope_queue_seen();
    }
}

void VulkanSubmissionExecutor::UpdateStreamFrontier(
    EQueueType _queue,
    WaitEvent  _completion,
    bool       _collapse
) {
    const RHIQueueTopology queue_topology =
        RenderDevice::Get().GetQueueTopology();
    if (_collapse) {
        gpu_frontier.fill(std::nullopt);
    } else {
        for (size_t index = 0; index < gpu_frontier.size(); ++index) {
            if (SameNativeQueue(queue_topology, static_cast<EQueueType>(index), _queue)) {
                gpu_frontier[index].reset();
            }
        }
    }
    gpu_frontier[static_cast<size_t>(_queue)] = _completion;
}

void VulkanSubmissionExecutor::LatchHardFailure(
    SubmissionPosition _position,
    int32              _result
) noexcept {
    std::lock_guard lock(hard_failure_mutex);
    const bool      earlier =
        !hard_failure_position || _position < *hard_failure_position;
    if (!earlier) {
        return;
    }
    hard_failure_position = _position;
    hard_failure_result =
        _result == static_cast<int32>(VK_SUCCESS) ?
            static_cast<int32>(VK_ERROR_UNKNOWN) :
            _result;
}

bool VulkanSubmissionExecutor::IsHardFailureAtOrBefore(
    SubmissionPosition _position,
    int32*             _result
) const noexcept {
    std::lock_guard lock(hard_failure_mutex);
    if (!hard_failure_position.has_value()) {
        return false;
    }
    const bool rejected =
        *hard_failure_position <= _position;
    if (rejected && _result != nullptr) {
        *_result = hard_failure_result;
    }
    return rejected;
}

std::optional<VulkanPendingSubmission> VulkanSubmissionExecutor::TranslateSourceForRuntime(
    EQueueType  _queue,
    CmdSubmit&& _submit
) noexcept {
    if (_queue == EQueueType::Copy) {
        auto& queue   = static_cast<VulkanCopyQueue&>(RenderDevice::Get().GetCopyQueue());
        auto  pending = queue.ReserveSubmission(std::move(_submit));
        queue.device.GetCommandRecorder(_queue).Record(pending);
        return pending;
    }
    if (_queue == EQueueType::Graphics || _queue == EQueueType::Compute) {
        auto& queue   = static_cast<VulkanCommandQueue&>(RenderDevice::Get().GetCommandQueue(_queue));
        auto  pending = queue.ReserveSubmission(std::move(_submit));
        queue.vk_device.GetCommandRecorder(_queue).Record(pending);
        return pending;
    }

    RejectSourceForRuntime(
        _queue,
        std::move(_submit),
        VK_ERROR_UNKNOWN,
        false
    );
    return {};
}

VulkanSubmissionResult VulkanSubmissionExecutor::SubmitRecordedSourceForRuntime(
    VulkanPendingSubmission _packet,
    BatchExecution*         _execution,
    size_t                  _reject_from
) noexcept {
    // Own the packet and execution lease through failure publication and the
    // transfer to Completion. No exception may unwind accepted GPU work.
    auto submit_and_enqueue_completion = [&](auto& _queue) noexcept {
        const VulkanSubmissionResult result = _queue.SubmitRecordedForRuntime(_packet);
        if (!result.outcome.Succeeded() && !result.WasSubmitted() && _execution != nullptr) {
            const int32 failure_result = result.outcome.result == VK_SUCCESS ?
                                             static_cast<int32>(VK_ERROR_UNKNOWN) :
                                             static_cast<int32>(result.outcome.result);
            const bool  recoverable    = result.IsRecoverableRejection();
            _execution->PublishSuffixRejection(
                _reject_from,
                failure_result,
                recoverable,
                recoverable ? "Vulkan submission suffix was rejected before Completion notification" :
                              "Vulkan submission suffix failed before Completion notification"
            );
        }
        if (!_packet.completion_enqueued) {
            _queue.CommitRuntimeSubmitCompletion(_packet, *_packet.submission_outcome);
        }
        return result;
    };

    const EQueueType queue_type = _packet.context.queue_type;
    if (queue_type == EQueueType::Copy) {
        auto& queue = static_cast<VulkanCopyQueue&>(RenderDevice::Get().GetCopyQueue());
        return submit_and_enqueue_completion(queue);
    }
    assert(queue_type == EQueueType::Graphics || queue_type == EQueueType::Compute);
    auto& queue = static_cast<VulkanCommandQueue&>(RenderDevice::Get().GetCommandQueue(queue_type));
    return submit_and_enqueue_completion(queue);
}

void VulkanSubmissionExecutor::RejectRecordedSourceForRuntime(
    VulkanPendingSubmission&& _packet,
    VkResult                  _result,
    bool                      _recoverable
) noexcept {
    if (_result == VK_SUCCESS) {
        _result = VK_ERROR_UNKNOWN;
    }
    const EQueueType queue_type = _packet.context.queue_type;
    if (queue_type == EQueueType::Copy) {
        auto& queue = static_cast<VulkanCopyQueue&>(RenderDevice::Get().GetCopyQueue());
        queue.RejectRecordedForRuntime(std::move(_packet), _result, _recoverable);
    } else {
        auto& queue = static_cast<VulkanCommandQueue&>(RenderDevice::Get().GetCommandQueue(queue_type));
        queue.RejectRecordedForRuntime(std::move(_packet), _result, _recoverable);
    }
}

void VulkanSubmissionExecutor::RejectSourceForRuntime(
    EQueueType  _queue,
    CmdSubmit&& _submit,
    VkResult    _result,
    bool        _recoverable
) noexcept {
    if (_queue == EQueueType::Copy) {
        auto& copy_queue =
            static_cast<VulkanCopyQueue&>(
                RenderDevice::Get().GetCopyQueue()
            );
        copy_queue.RejectForRuntime(
            std::move(_submit),
            _result,
            _recoverable
        );
        return;
    }
    if (_queue == EQueueType::Graphics ||
        _queue == EQueueType::Compute) {
        auto& queue = static_cast<VulkanCommandQueue&>(
            RenderDevice::Get().GetCommandQueue(_queue)
        );
        queue.RejectForRuntime(
            std::move(_submit),
            _result,
            _recoverable
        );
        return;
    }

    try {
        LOG_ERROR(
            "[RHIExecutor][Vulkan] cannot terminalize unsupported queue type={}",
            static_cast<uint32>(_queue)
        );
    } catch (...) {
    }
}

VkResult VulkanSubmissionExecutor::GetQueueFaultResult(
    EQueueType _queue
) const noexcept {
    if (_queue == EQueueType::Copy) {
        auto& copy_queue =
            static_cast<VulkanCopyQueue&>(
                RenderDevice::Get().GetCopyQueue()
            );
        return copy_queue.device.IsFaulted() ?
                   copy_queue.device.GetFirstFaultResult() :
                   VK_ERROR_UNKNOWN;
    }
    if (_queue == EQueueType::Graphics ||
        _queue == EQueueType::Compute) {
        auto& queue = static_cast<VulkanCommandQueue&>(
            RenderDevice::Get().GetCommandQueue(_queue)
        );
        return queue.vk_device.IsFaulted() ?
                   queue.vk_device.GetFirstFaultResult() :
                   VK_ERROR_UNKNOWN;
    }
    return VK_ERROR_UNKNOWN;
}

void VulkanSubmissionExecutor::ExecutePipelineSource(
    const std::shared_ptr<BatchExecution>& _batch,
    size_t                                 _source_index
) noexcept {
    auto finish_work = OnScopeExit([&]() noexcept {
        _batch->FinishWork();
    });

    if (_source_index >= _batch->sources.size()) {
        LatchHardFailure(
            SubmissionPosition{_batch->sequence, _source_index},
            static_cast<int32>(VK_ERROR_UNKNOWN)
        );
        return;
    }

    SubmissionSource& slot = _batch->sources[_source_index];
    auto              reject_packet =
        [&](VkResult         _result,
            bool             _recoverable,
            std::string_view _reason) noexcept {
            if (_result == VK_SUCCESS) {
                _result = VK_ERROR_UNKNOWN;
            }
            {
                _batch->PublishSuffixRejection(
                    _source_index,
                    static_cast<int32>(_result),
                    _recoverable,
                    _reason
                );
            }
            if (slot.recorded.has_value()) {
                RejectRecordedSourceForRuntime(
                    std::move(*slot.recorded),
                    _result,
                    _recoverable
                );
                slot.recorded.reset();
            }
        };

    int32 hard_result = static_cast<int32>(VK_ERROR_UNKNOWN);
    if (IsHardFailureAtOrBefore(
            SubmissionPosition{_batch->sequence, _source_index},
            &hard_result
        )) {
        reject_packet(
            static_cast<VkResult>(hard_result),
            false,
            "Vulkan pipeline source was rejected after a hard failure"
        );
        return;
    }
    if (auto failure = _batch->ReadFailure(_source_index)) {
        reject_packet(
            failure->outcome.result,
            failure->IsRecoverableRejection(),
            failure->IsRecoverableRejection() ?
                "Vulkan pipeline source was rejected by a dependency" :
                "Vulkan pipeline source was rejected after a hard failure"
        );
        return;
    }

    if (!slot.recorded.has_value()) {
        {
            _batch->PublishSuffixRejection(
                _source_index,
                static_cast<int32>(VK_ERROR_UNKNOWN),
                false,
                "Vulkan pipeline source lost its recorded packet"
            );
        }
        LatchHardFailure(
            SubmissionPosition{_batch->sequence, _source_index + 1},
            static_cast<int32>(VK_ERROR_UNKNOWN)
        );
        _batch->PublishFailure(
            _source_index + 1,
            static_cast<int32>(VK_ERROR_UNKNOWN),
            false
        );
        ResetStreamScope();
        return;
    }
    try {
        PrepareStreamSubmit(slot.recorded->submit, slot.queue_type);
    } catch (...) {
        ReportRequestFailure(
            ERequestKind::Submit,
            VulkanSubmissionDetail::EWorkerRequestFailurePhase::Process,
            std::current_exception()
        );
        reject_packet(
            VK_ERROR_UNKNOWN,
            false,
            "Vulkan pipeline source preparation failed"
        );
        _batch->PublishFailure(
            _source_index + 1,
            static_cast<int32>(VK_ERROR_UNKNOWN),
            false
        );
        LatchHardFailure(
            SubmissionPosition{_batch->sequence, _source_index + 1},
            static_cast<int32>(VK_ERROR_UNKNOWN)
        );
        ResetStreamScope();
        return;
    }

    VulkanSubmissionResult submit_result =
        SubmitRecordedSourceForRuntime(
            std::move(*slot.recorded), _batch.get(), _source_index
        );
    slot.recorded.reset();

    if (submit_result.WasSubmitted()) {
        assert(submit_result.completion.has_value());
#if MOER_VULKAN_TEST_SUPPORT
        NotifyVulkanSourceSubmitted(
            _batch->sequence,
            static_cast<uint32>(_source_index),
            slot.original_source_index,
            slot.source_segment_index,
            slot.source_segment_count,
            slot.queue_type,
            slot.async_queue_scope,
            slot.cross_native_predecessor_wait
        );
#endif
        AdvanceSourceFrontier(slot, *submit_result.completion);
        return;
    }

    const int32 failure_result =
        submit_result.outcome.result == VK_SUCCESS ?
            static_cast<int32>(VK_ERROR_UNKNOWN) :
            static_cast<int32>(submit_result.outcome.result);
    const bool recoverable = submit_result.IsRecoverableRejection();
    _batch->PublishFailure(
        _source_index + 1, failure_result, recoverable
    );
    ResetStreamScope();
    if (!recoverable) {
        LatchHardFailure(
            SubmissionPosition{_batch->sequence, _source_index + 1},
            failure_result
        );
    }
}

void VulkanSubmissionExecutor::ReportRequestFailure(
    ERequestKind                                       _kind,
    VulkanSubmissionDetail::EWorkerRequestFailurePhase _phase,
    const std::exception_ptr&                          _exception
) noexcept {
    const char* kind_name = "Submit";
    switch (_kind) {
        case ERequestKind::Submit:
            break;
        case ERequestKind::Sync:
            kind_name = "Sync";
            break;
        case ERequestKind::Stop:
            kind_name = "Stop";
            break;
    }

    const char* phase_name = "process";
    switch (_phase) {
        case VulkanSubmissionDetail::EWorkerRequestFailurePhase::Process:
            break;
        case VulkanSubmissionDetail::EWorkerRequestFailurePhase::Reject:
            phase_name = "reject";
            break;
        case VulkanSubmissionDetail::EWorkerRequestFailurePhase::Complete:
            phase_name = "complete";
            break;
    }

    try {
        if (_exception) {
            std::rethrow_exception(_exception);
        }
    } catch (const std::exception& error) {
        try {
            LOG_ERROR(
                "[RHIExecutor][Vulkan] request={} phase={} threw: {}",
                kind_name,
                phase_name,
                error.what()
            );
        } catch (...) {
        }
        return;
    } catch (...) {
    }

    try {
        LOG_ERROR(
            "[RHIExecutor][Vulkan] request={} phase={} threw an unknown exception",
            kind_name,
            phase_name
        );
    } catch (...) {
    }
}

void VulkanSubmissionExecutor::RejectBatch(
    RHISubmissionBatch&& _batch,
    int32                _result,
    std::string_view     _reason,
    bool                 _recoverable,
    size_t               _first_source,
    size_t*              _next_unconsumed_source
) {
    const size_t   first_source = std::min(_first_source, _batch.submits.size());
    const VkResult result =
        _result == static_cast<int32>(VK_SUCCESS) ?
            VK_ERROR_UNKNOWN :
            static_cast<VkResult>(_result);

    const GpuCompletionPublishBatch completion_batch =
        GpuCompletionBackendAccess::BeginPublishBatch();
    for (size_t source_index = first_source;
         source_index < _batch.submits.size();
         ++source_index) {
        CmdSubmit&                      submit = _batch.submits[source_index].submit;
        const GpuCompletionPublishBatch source_completion_batch =
            submit.gpu_completion_publish_batch.Valid() ?
                submit.gpu_completion_publish_batch :
                completion_batch;
        submit.PublishPendingGpuCompletionErrors(
            _reason, source_completion_batch
        );
    }

    const QueryPublishBatch query_batch =
        QueryBackendAccess::BeginPublishBatch();
    for (size_t source_index = first_source;
         source_index < _batch.submits.size();
         ++source_index) {
        CmdSubmit& submit = _batch.submits[source_index].submit;
        // Batch admission may already have installed the winning notification
        // ticket. Rejection must publish through that owner instead of
        // replacing it with the fallback shared by previously unsnapshotted
        // entries.
        const QueryPublishBatch source_query_batch =
            submit.query_publish_batch.Valid() ?
                submit.query_publish_batch :
                query_batch;
        submit.PublishPendingQueryErrors(_reason, source_query_batch);
    }

    // Terminal publication is atomic at the runtime-batch frontier. Publish
    // every host-visible state first so synchronous Fence rejection observers
    // cannot block before their matching Future/Query becomes terminal.
    for (size_t source_index = first_source;
         source_index < _batch.submits.size();
         ++source_index) {
        CmdSubmit& submit = _batch.submits[source_index].submit;
        if (_recoverable) {
            submit.RejectPendingSignals();
            continue;
        }
        Array<SignalEvent> rejected_signals =
            std::move(submit.signal_events);
        Array<FenceRef> rejected_signal_keepalives =
            std::move(submit.signal_rejection_keepalives);
        submit.signal_events.clear();
        submit.signal_rejection_keepalives.clear();
        for (const SignalEvent& signal : rejected_signals) {
            auto* fence =
                reinterpret_cast<VulkanTimeline*>(signal.timeline_handle);
            if (fence == nullptr) {
                continue;
            }
            try {
                fence->Fail(result);
            } catch (...) {
                fence->Reject(signal.value);
            }
        }
        (void)rejected_signal_keepalives;
    }

    // A Present receipt is also user-visible batch state. Resolve it only
    // after every host state and signal in the rejected suffix is terminal,
    // so a receipt waiter cannot observe a partially published batch.
    ResolveRejectedPresent(_batch.present);

    for (size_t source_index = first_source;
         source_index < _batch.submits.size();
         ++source_index) {
        RHISubmissionRequest& entry = _batch.submits[source_index];
        if (entry.queue == EQueueType::Copy) {
            auto& queue = static_cast<VulkanCopyQueue&>(RenderDevice::Get().GetCopyQueue());
            queue.RejectForRuntime(
                std::move(entry.submit),
                result,
                _recoverable
            );
            if (_next_unconsumed_source != nullptr) {
                *_next_unconsumed_source = source_index + 1;
            }
            continue;
        }

        const EQueueType queue_type =
            IsNativeQueue(entry.queue) && entry.queue != EQueueType::Copy ?
                entry.queue :
                EQueueType::Graphics;
        auto& queue = static_cast<VulkanCommandQueue&>(
            RenderDevice::Get().GetCommandQueue(queue_type)
        );
        queue.RejectForRuntime(
            std::move(entry.submit),
            result,
            _recoverable
        );
        if (_next_unconsumed_source != nullptr) {
            *_next_unconsumed_source = source_index + 1;
        }
    }

    LOG_ERROR(
        "[RHIExecutor][Vulkan] reject batch={} sources={} reason={}",
        _batch.sequence,
        _batch.submits.size() - first_source,
        _reason
    );
}

bool VulkanSubmissionExecutor::PrepareBatch(
    RHISubmissionBatch&                 _batch,
    size_t&                             _first_unconsumed_source,
    VulkanSegmentMaterializationResult& materialization
) {
    int32 latched_failure_result = static_cast<int32>(VK_ERROR_UNKNOWN);
    if (IsHardFailureAtOrBefore(
            SubmissionPosition{_batch.sequence, 0},
            &latched_failure_result
        )) {
        DrainPipelineBatches();
        RejectBatch(
            std::move(_batch),
            latched_failure_result,
            "submission runtime is latched after a hard failure",
            false,
            0,
            &_first_unconsumed_source
        );
        return false;
    }

    const BatchPreflightResult preflight = ValidateBatch(_batch);
    if (!preflight.valid) {
        LOG_ERROR(
            "[RHIExecutor][Topology] batch={} preflight rejected reason={} "
            "topology_error={} source={} command={}",
            _batch.sequence,
            preflight.reason,
            TopologyErrorName(_batch.topology.error),
            preflight.source_index,
            preflight.command_index
        );
#if MOER_VULKAN_TEST_SUPPORT
        NotifyVulkanBatchPreflightRejected(_batch.sequence, false);
#endif
        // Validation runs before the ordinary admission decision so a later
        // malformed batch can be discovered while an earlier pipeline batch
        // is still owned by Submission. Commit that accepted prefix before
        // its queue receives this batch's rejection markers.
        DrainPipelineBatches();
        LatchHardFailure(
            SubmissionPosition{_batch.sequence, 0},
            static_cast<int32>(VK_ERROR_UNKNOWN)
        );
        RejectBatch(
            std::move(_batch),
            static_cast<int32>(VK_ERROR_UNKNOWN),
            preflight.reason,
            true,
            0,
            &_first_unconsumed_source
        );
        return false;
    }

    materialization = MaterializeMultiSegmentBatch(_batch, RenderDevice::Get().GetQueueTopology());
    if (materialization.changed) {
        const BatchPreflightResult executable_preflight = ValidateBatch(_batch);
        if (!executable_preflight.valid) {
            LOG_ERROR(
                "[RHIExecutor][Vulkan][MultiSegment] batch={} "
                "executable preflight rejected reason={} topology_error={} "
                "source={} command={}",
                _batch.sequence,
                executable_preflight.reason,
                TopologyErrorName(_batch.topology.error),
                executable_preflight.source_index,
                executable_preflight.command_index
            );
#if MOER_VULKAN_TEST_SUPPORT
            NotifyVulkanBatchPreflightRejected(_batch.sequence, true);
#endif
            DrainPipelineBatches();
            LatchHardFailure(
                SubmissionPosition{_batch.sequence, 0},
                static_cast<int32>(VK_ERROR_UNKNOWN)
            );
            RejectBatch(
                std::move(_batch),
                static_cast<int32>(VK_ERROR_UNKNOWN),
                executable_preflight.reason,
                true,
                0,
                &_first_unconsumed_source
            );
            return false;
        }
        if (!logged_multi_segment_topology) {
            logged_multi_segment_topology = true;
            LOG_INFO(
                "[RHIExecutor][Vulkan][MultiSegment] "
                "original_sources={} executable_segments={} "
                "cross_native_waits={} completion_aggregate=true",
                materialization.original_source_count,
                _batch.submits.size(),
                materialization.cross_native_wait_count
            );
        }
    }

    if (!logged_multi_source_topology && _batch.submits.size() > 1) {
        logged_multi_source_topology = true;
        LOG_INFO(
            "[RHIExecutor][Vulkan] upper submission topology active: batch={} "
            "sources={} segments={} present={}",
            _batch.sequence,
            _batch.submits.size(),
            _batch.topology.segments.size(),
            _batch.present.has_value()
        );
    }

    return true;
}

bool VulkanSubmissionExecutor::CanPipelineBatch(const RHISubmissionBatch& _batch) const {
    const auto queue_topology                    = RenderDevice::Get().GetQueueTopology();
    bool       batch_crosses_aliased_native_lane = false;
    if (runtime_queues_share_native_lane) {
        for (size_t lhs = 0;
             lhs < _batch.submits.size() &&
             !batch_crosses_aliased_native_lane;
             ++lhs) {
            const EQueueType      lhs_queue = _batch.submits[lhs].queue;
            const RHIQueueBinding lhs_binding =
                queue_topology.Resolve(lhs_queue);
            for (size_t rhs = lhs + 1;
                 rhs < _batch.submits.size();
                 ++rhs) {
                const EQueueType rhs_queue =
                    _batch.submits[rhs].queue;
                if (lhs_queue == rhs_queue) {
                    continue;
                }
                const RHIQueueBinding rhs_binding =
                    queue_topology.Resolve(rhs_queue);
                if (lhs_binding.available &&
                    rhs_binding.available &&
                    lhs_binding.native_queue_id ==
                        rhs_binding.native_queue_id) {
                    batch_crosses_aliased_native_lane = true;
                    break;
                }
            }
        }
    }
    return !_batch.present.has_value() && !_batch.submits.empty() && !batch_crosses_aliased_native_lane &&
           std::all_of(_batch.submits.begin(), _batch.submits.end(), [](const RHISubmissionRequest& entry) {
               return IsParallelTranslateCandidate(entry) && !entry.submit.b_sync && !entry.submit.b_tick_profiling &&
                      !entry.submit.b_delete_resources && !entry.submit.EmitsProfilingQueries();
           });
}

bool VulkanSubmissionExecutor::PrepareSource(RHISubmissionBatch& _batch, size_t _source_index, bool _pipeline) {
    auto&       entry = _batch.submits[_source_index];
    const auto& plan  = _batch.topology.source_plans[_source_index];
    if (plan.segment_plan_count == 0) {
        return false;
    }
    assert(plan.segment_plan_count == 1);
    const auto& segment   = _batch.topology.segments[plan.segment_plan_begin];
    entry.submit.segments = {RHISubmitSegment{
        .queue = segment.segment.queue,
        .begin = 0,
        .end   = entry.submit.cmds.size(),
    }};
    if (!_pipeline) {
        PrepareStreamSubmit(entry.submit, entry.queue);
    }
    return true;
}

void VulkanSubmissionExecutor::DrainBatchSubmissionTasks(const std::shared_ptr<BatchExecution>& _execution) {
    if (_execution->submission_tasks_done) {
        // Seal before waiting: this batch is itself among the pending futures.
        _execution->Seal();
        DrainPipelineBatches();
    }
}

void VulkanSubmissionExecutor::RejectRemainingSources(
    RHISubmissionBatch&                    _batch,
    const std::shared_ptr<BatchExecution>& _execution,
    size_t                                 _begin,
    VkResult                               _result,
    std::string_view                       _reason,
    bool                                   _recoverable,
    size_t&                                _first_unconsumed_source
) {
    if (_result == VK_SUCCESS) {
        _result = VK_ERROR_UNKNOWN;
    }
    _execution->PublishSuffixRejection(_begin, static_cast<int32>(_result), _recoverable, _reason);
    if (_execution->submission_tasks_done) {
        _execution->PublishFailure(_begin, static_cast<int32>(_result), _recoverable);
        DrainBatchSubmissionTasks(_execution);
    } else {
        // Preserve accepted GPU tails while ending the failed async scope.
        ResetStreamScope();
    }
    if (!_recoverable) {
        LatchHardFailure({_batch.sequence, _begin}, static_cast<int32>(_result));
    }
    _first_unconsumed_source = std::min(_begin, _batch.submits.size());
    RejectBatch(std::move(_batch), static_cast<int32>(_result), _reason, _recoverable, _first_unconsumed_source, &_first_unconsumed_source);
    _first_unconsumed_source = _batch.submits.size();
}

bool VulkanSubmissionExecutor::EnqueuePipelineSource(const std::shared_ptr<BatchExecution>& _execution, size_t _source_index) {
    _execution->AddWork();
    try {
        if (EnqueueSubmissionWork(SubmissionWork([this, state = _execution, _source_index] {
                ExecutePipelineSource(state, _source_index);
            }))) {
            return true;
        }
    } catch (...) {
        ReportRequestFailure(ERequestKind::Submit, VulkanSubmissionDetail::EWorkerRequestFailurePhase::Process, std::current_exception());
    }
    _execution->FinishWork();
    return false;
}

void VulkanSubmissionExecutor::RecordSource(uint64 _sequence, SubmissionSource& _source, CmdSubmit&& _submit) noexcept {
    _source.attempted = true;
#if MOER_VULKAN_TEST_SUPPORT
    const uint32 native_queue_id = RenderDevice::Get().GetQueueTopology().Resolve(_source.queue_type).native_queue_id;
    auto         notify          = [&](EVulkanSourceTranslationPhase phase) noexcept {
        NotifyVulkanSourceTranslation(
            _sequence,
            static_cast<uint32>(_source.source_index),
            _source.original_source_index,
            _source.source_segment_index,
            _source.source_segment_count,
            _source.queue_type,
            native_queue_id,
            _source.async_queue_scope,
            phase
        );
    };
    notify(EVulkanSourceTranslationPhase::Begin);
#else
    (void)_sequence;
#endif
    _source.recorded = TranslateSourceForRuntime(_source.queue_type, std::move(_submit));
#if MOER_VULKAN_TEST_SUPPORT
    notify(_source.recorded ? EVulkanSourceTranslationPhase::Recorded : EVulkanSourceTranslationPhase::Failed);
#endif
}

bool VulkanSubmissionExecutor::TranslateWave(RHISubmissionBatch& _batch, std::span<SubmissionSource*> _sources) {
    if (_sources.size() <= 1 || !TaskGraph::IsInitialized()) {
        for (auto* source : _sources) {
            RecordSource(_batch.sequence, *source, std::move(_batch.submits[source->source_index].submit));
        }
        return true;
    }
    // Join even after dispatch allocation fails: queued workers retain references
    // to this latch and to the batch until they have all finished recording.
    std::latch completed(static_cast<std::ptrdiff_t>(_sources.size()));
    size_t     dispatched = 0;
    bool       succeeded  = true;
    for (; dispatched < _sources.size(); ++dispatched) {
        auto* source = _sources[dispatched];
        try {
            auto* entry = &_batch.submits[source->source_index];
            (void)LambdaTask::Dispatch([this, source, entry, &completed, sequence = _batch.sequence] {
                RHIThreadRoleScope translate_worker(ERHIThreadRole::Translate);
                MOER_PROFILE_SCOPE("Vulkan.TranslateSubmission");
                RecordSource(sequence, *source, std::move(entry->submit));
                completed.count_down();
            },
                                       EThread::AnyThread_NormalPri);
        } catch (...) {
            succeeded = false;
            completed.count_down(static_cast<std::ptrdiff_t>(_sources.size() - dispatched));
            break;
        }
    }
    completed.wait();
    return succeeded;
}

VulkanSubmissionResult VulkanSubmissionExecutor::SubmitSourceAndWait(
    const std::shared_ptr<BatchExecution>& _execution,
    size_t                                 _source_index,
    VulkanPendingSubmission&               _packet
) {
    try {
        return RunOnSubmissionThreadAndWait([this, execution = _execution, _source_index, &_packet] {
            auto submit = [&] {
                return SubmitRecordedSourceForRuntime(std::move(_packet), execution.get(), _source_index);
            };
#if MOER_VULKAN_TEST_SUPPORT
            const bool                             serial_control = _packet.submit.translate_execution_class == ERHITranslateExecutionClass::SerialControl;
            const auto&                            source         = execution->sources[_source_index];
            const VulkanSubmissionBoundaryIdentity boundary{
                .batch_sequence  = execution->sequence,
                .operation_index = static_cast<uint32>(_source_index),
                .queue           = source.queue_type,
                .kind            = EVulkanSubmissionBoundaryKind::SerialControl,
            };
            const auto result = serial_control ? ExecuteObservedSubmissionBoundary(boundary, submit) : submit();
            if (result.WasSubmitted()) {
                NotifyVulkanSourceSubmitted(
                    execution->sequence,
                    static_cast<uint32>(_source_index),
                    source.original_source_index,
                    source.source_segment_index,
                    source.source_segment_count,
                    source.queue_type,
                    source.async_queue_scope,
                    source.cross_native_predecessor_wait
                );
            }
            return result;
#else
            return submit();
#endif
        });
    } catch (...) {
        ReportRequestFailure(ERequestKind::Submit, VulkanSubmissionDetail::EWorkerRequestFailurePhase::Process, std::current_exception());
        _execution->PublishSuffixRejection(_source_index, static_cast<int32>(VK_ERROR_UNKNOWN), false, "Submission handoff failed before Completion notification");
        RejectRecordedSourceForRuntime(std::move(_packet), VK_ERROR_UNKNOWN, false);
        return {.outcome = {EVulkanOperationStatus::Rejected, VK_ERROR_UNKNOWN}};
    }
}

void VulkanSubmissionExecutor::AdvanceSourceFrontier(const SubmissionSource& _source, WaitEvent _completion) {
    if (_source.queue_type == EQueueType::Copy) {
        last_copy_timeline = std::max(last_copy_timeline, _completion.value);
    }
    UpdateStreamFrontier(_source.queue_type, _completion, _source.async_queue_scope == 0);
}

bool VulkanSubmissionExecutor::ProcessSerialSource(
    RHISubmissionBatch&                    _batch,
    const std::shared_ptr<BatchExecution>& _execution,
    size_t                                 _source_index,
    size_t&                                _first_unconsumed_source
) {
    if (!PrepareSource(_batch, _source_index, false)) {
        _first_unconsumed_source = _source_index + 1;
        return true;
    }
    auto& entry    = _batch.submits[_source_index];
    auto  recorded = TranslateSourceForRuntime(entry.queue, std::move(entry.submit));
    if (!recorded) {
        RejectRemainingSources(_batch, _execution, _source_index, GetQueueFaultResult(entry.queue), "serial command translation failure", false, _first_unconsumed_source);
        return false;
    }
    const auto result = SubmitSourceAndWait(_execution, _source_index, *recorded);
    // The current source has been submitted or terminalized. Later exceptions
    // must only reject the remaining suffix.
    _first_unconsumed_source = _source_index + 1;
    if (result.WasSubmitted()) {
        AdvanceSourceFrontier(_execution->sources[_source_index], *result.completion);
    } else if (result.IsRecoverableRejection() || result.IsHardFailure()) {
        RejectRemainingSources(
            _batch,
            _execution,
            _source_index + 1,
            result.outcome.result,
            "command submission failed",
            result.IsRecoverableRejection(),
            _first_unconsumed_source
        );
        return false;
    }
    return true;
}

bool VulkanSubmissionExecutor::ProcessParallelSources(
    RHISubmissionBatch&                    _batch,
    const std::shared_ptr<BatchExecution>& execution,
    size_t                                 group_begin,
    size_t                                 group_end,
    size_t&                                _first_unconsumed_source
) {
    const bool pipeline_mode  = execution->submission_tasks_done.has_value();
    const auto queue_topology = RenderDevice::Get().GetQueueTopology();
    using namespace VulkanTranslateWaveDetail;

    assert(group_begin < group_end);
    assert(group_end <= _batch.submits.size());

    const size_t                group_size = group_end - group_begin;
    Array<Array<SubmissionKey>> dependency_storage(group_size);
    Array<TranslateWaveNode>    schedule_nodes{};
    schedule_nodes.reserve(group_size);

    const size_t first_segment = _batch.topology.source_plans[group_begin].segment_plan_begin;
    const size_t last_segment  = _batch.topology.source_plans[group_end - 1].segment_plan_begin;
    for (size_t local_index = 0; local_index < group_size; ++local_index) {
        const size_t               source_index = group_begin + local_index;
        const RHISourceSubmitPlan& source_plan  = _batch.topology.source_plans[source_index];
        assert(source_plan.segment_plan_count == 1);
        const RHISubmissionSegmentPlan& segment_plan =
            _batch.topology.segments[source_plan.segment_plan_begin];

        Array<SubmissionKey>& dependencies = dependency_storage[local_index];
        dependencies.reserve(segment_plan.dependencies.size());
        for (const SubmissionKey& dependency : segment_plan.dependencies) {
            // A dependency outside this contiguous explicit-RDG
            // group belongs to an already terminalized prefix.
            if (dependency.submit_idx >= first_segment && dependency.submit_idx <= last_segment) {
                dependencies.emplace_back(dependency);
            }
        }

        schedule_nodes.emplace_back(TranslateWaveNode{
            .key             = segment_plan.key,
            .native_queue_id = queue_topology.Resolve(_batch.submits[source_index].queue).native_queue_id,
            .async_translate = true,
            .dependencies    = dependencies,
        });
    }

    TranslateWaveScheduler scheduler{};
    if (!scheduler.Build(schedule_nodes)) {
        const size_t error_node = scheduler.GetErrorNodeIndex();
        LOG_ERROR(
            "[RHIExecutor][Vulkan][ParallelTranslate] "
            "schedule rejected batch={} source={} error={} "
            "dependency={}",
            _batch.sequence,
            error_node == TranslateWaveScheduler::NoIndex ? group_begin : group_begin + error_node,
            static_cast<uint32>(scheduler.GetBuildError()),
            scheduler.GetErrorDependencyIndex()
        );
        RejectRemainingSources(_batch, execution, group_begin, VK_ERROR_UNKNOWN, "parallel Translate schedule rejected", false, _first_unconsumed_source);
        return false;
    }

    auto slots          = std::span(execution->sources).subspan(group_begin, group_size);
    bool group_finished = false;
    auto finish_group   = [&]() noexcept {
        _first_unconsumed_source = std::max(_first_unconsumed_source, group_end);
        group_finished           = true;
    };
    // The scope guard closes every packet still owned by Translate, after
    // earlier handed-off work has published its Submission decisions.
    auto reject_group = [&](VkResult result) noexcept {
        if (group_finished) {
            return;
        }
        if (result == VK_SUCCESS) {
            result = VK_ERROR_UNKNOWN;
        }
        if (pipeline_mode) {
            DrainBatchSubmissionTasks(execution);
        }
        size_t rejection_begin = group_end;
        for (const auto& slot : slots) {
            if (!slot.handed_off) {
                rejection_begin = std::min(rejection_begin, slot.source_index);
            }
        }
        if (rejection_begin < group_end) {
            execution->PublishSuffixRejection(rejection_begin, static_cast<int32>(result), false, "parallel Translate group failed before Completion notification");
        }
        for (auto& slot : slots) {
            if (slot.handed_off) {
                continue;
            }
            auto& entry = _batch.submits[slot.source_index];
            if (slot.recorded) {
                RejectRecordedSourceForRuntime(std::move(*slot.recorded), result, false);
                slot.recorded.reset();
            } else if (!slot.attempted) {
                RejectSourceForRuntime(slot.queue_type, std::move(entry.submit), result, false);
            }
            slot.handed_off = true;
        }
        finish_group();
    };
    auto reject_on_exit = OnScopeExit([&]() noexcept {
        reject_group(VK_ERROR_UNKNOWN);
    });

    auto slot_for_key = [&](const SubmissionKey& key) -> SubmissionSource& {
        assert(key.op_seq == _batch.sequence);
        assert(key.submit_idx >= first_segment);
        const size_t local_index = key.submit_idx - first_segment;
        assert(local_index < slots.size());
        assert(slots[local_index].key == key);
        return slots[local_index];
    };
    auto fail_group = [&](size_t                failure_source,
                          VkResult              failure_result,
                          bool                  recoverable_failure,
                          std::string_view      failure_reason,
                          std::optional<size_t> rejection_source =
                              std::nullopt) {
        if (failure_result == VK_SUCCESS) {
            failure_result = VK_ERROR_UNKNOWN;
        }
        scheduler.Cancel();
        size_t rejection_begin = group_end;
        for (const SubmissionSource& slot : slots) {
            if (!slot.handed_off) {
                rejection_begin =
                    std::min(rejection_begin, slot.source_index);
            }
        }
        // Include the failed source even when Translate could not return
        // a terminal packet. Its Query state must be published before any
        // earlier same-queue callback can run.
        rejection_begin = std::min(
            rejection_begin,
            rejection_source.value_or(failure_source)
        );
        execution->PublishSuffixRejection(
            rejection_begin,
            static_cast<int32>(failure_result),
            recoverable_failure,
            failure_reason
        );
        reject_group(failure_result);
        LOG_ERROR(
            "[RHIExecutor][Vulkan][ParallelTranslate] "
            "batch={} failure_source={} reason={} result={} "
            "recoverable={}",
            _batch.sequence,
            failure_source,
            failure_reason,
            static_cast<int32>(failure_result),
            recoverable_failure
        );
        RejectRemainingSources(_batch, execution, group_end, failure_result, failure_reason, recoverable_failure, _first_unconsumed_source);
        return false;
    };

    size_t next_release_local = 0;
    for (;;) {
        std::vector<SubmissionKey> wave = scheduler.NextWave();
        if (wave.empty()) {
            if (scheduler.IsComplete()) {
                assert(next_release_local == slots.size());
                finish_group();
                return true;
            }
            return fail_group(
                group_begin + next_release_local,
                VK_ERROR_UNKNOWN,
                false,
                "parallel Translate schedule stalled"
            );
        }

        Array<SubmissionSource*> wave_slots{};
        wave_slots.reserve(wave.size());
        for (const SubmissionKey& key : wave) {
            SubmissionSource& slot = slot_for_key(key);
            if (!PrepareSource(_batch, slot.source_index, pipeline_mode)) {
                return fail_group(
                    slot.source_index,
                    VK_ERROR_UNKNOWN,
                    false,
                    "parallel Translate source lost its segment"
                );
            }
            wave_slots.emplace_back(&slot);
        }

        const bool dispatch_failed = !TranslateWave(_batch, wave_slots);

        std::optional<size_t> translation_failure_source{};
        VkResult              translation_failure_result = VK_ERROR_UNKNOWN;
        std::string_view      translation_failure_reason{"parallel Translate task dispatch failure"};
        for (SubmissionSource* slot : wave_slots) {
            if (!slot->attempted) {
                if (!translation_failure_source) {
                    translation_failure_source = slot->source_index;
                    translation_failure_reason = "parallel Translate task dispatch failure";
                }
                continue;
            }

            if (!scheduler.MarkTranslated(slot->key) && !translation_failure_source) {
                translation_failure_source = slot->source_index;
                translation_failure_reason = "parallel Translate scheduler state failure";
            }
            if (!slot->recorded.has_value()) {
                // Runtime Translate normally returns a terminal packet
                // for native-record failures. A missing packet is an
                // unexpected handoff failure and has no Completion packet
                // left for the group terminalizer to retire.
                slot->handed_off = true;
                if (!translation_failure_source) {
                    translation_failure_source = slot->source_index;
                    translation_failure_result =
                        GetQueueFaultResult(slot->queue_type);
                    translation_failure_reason = "parallel command translation failure";
                }
            }
        }

        if (dispatch_failed || translation_failure_source) {
            return fail_group(
                translation_failure_source.value_or(group_begin + next_release_local),
                translation_failure_result,
                false,
                translation_failure_reason
            );
        }

        if (!logged_parallel_translate_wave && wave_slots.size() > 1) {
            logged_parallel_translate_wave = true;
            LOG_INFO(
                "[RHIExecutor][Vulkan][ParallelTranslate] "
                "mode=ready-native-lanes workers=TaskGraph "
                "native_lanes={} ordered_submit_owner=serial "
                "batch_window={}",
                wave_slots.size(),
                batch_window
            );
        }

        // Translation readiness is a native-lane ready set, while
        // submission remains a distinct stable cursor. A later
        // independent lane may already hold a recorded packet here;
        // it is released only after every earlier source is ready.
        while (next_release_local < slots.size()) {
            SubmissionSource& slot  = slots[next_release_local];
            const auto        state = scheduler.GetState(slot.key);
            if (!state || *state != ETranslateWaveNodeState::Translated) {
                break;
            }
            assert(slot.recorded.has_value());
            assert(!slot.handed_off);

            if (pipeline_mode) {
                if (!EnqueuePipelineSource(execution, slot.source_index)) {
                    return fail_group(slot.source_index, VK_ERROR_UNKNOWN, false, "parallel Translate pipeline handoff failure");
                }

                slot.handed_off = true;
                _first_unconsumed_source =
                    slot.source_index + 1;
                if (!scheduler.MarkReleased(slot.key)) {
                    return fail_group(
                        slot.source_index,
                        VK_ERROR_UNKNOWN,
                        false,
                        "parallel Translate scheduler handoff release failure",
                        slot.source_index + 1
                    );
                }
                ++next_release_local;
                continue;
            }

            const auto submit_result = SubmitSourceAndWait(execution, slot.source_index, *slot.recorded);
            slot.recorded.reset();
            slot.handed_off          = true;
            _first_unconsumed_source = slot.source_index + 1;
            if (!scheduler.MarkReleased(slot.key)) {
                return fail_group(
                    slot.source_index,
                    VK_ERROR_UNKNOWN,
                    false,
                    "parallel Translate scheduler release failure",
                    submit_result.WasSubmitted() ?
                        slot.source_index + 1 :
                        slot.source_index
                );
            }
            ++next_release_local;

            if (submit_result.WasSubmitted()) {
                AdvanceSourceFrontier(slot, *submit_result.completion);
                continue;
            }

            const VkResult         failure_result      = submit_result.outcome.result == VK_SUCCESS ?
                                                             VK_ERROR_UNKNOWN :
                                                             submit_result.outcome.result;
            const bool             recoverable_failure = submit_result.IsRecoverableRejection();
            const std::string_view failure_reason =
                recoverable_failure ? "parallel command submission dependency rejection" :
                                      "parallel command submission hard failure";
            return fail_group(slot.source_index, failure_result, recoverable_failure, failure_reason);
        }
    }
}

void VulkanSubmissionExecutor::ProcessPresent(
    RHISubmissionBatch&                    _batch,
    const std::shared_ptr<BatchExecution>& _execution,
    size_t&                                _first_unconsumed_source
) {
    ResetStreamScope();
    auto&     present        = *_batch.present;
    auto&     graphics_queue = static_cast<VulkanCommandQueue&>(RenderDevice::Get().GetCommandQueue(EQueueType::Graphics));
    CmdSubmit bridge         = CommandList(EQueueType::Graphics).Submit();
    AppendFrontierWaits(bridge, EQueueType::Graphics, gpu_frontier);
    // Aliased logical queues need no GPU semaphore wait, but their independent
    // Completion owners still require a Graphics retirement marker.
    if (!bridge.wait_events.empty() || HasForeignAliasedFrontier(EQueueType::Graphics)) {
#if MOER_VULKAN_TEST_SUPPORT
        const VulkanSubmissionBoundaryIdentity boundary{
            .batch_sequence        = _batch.sequence,
            .operation_index       = static_cast<uint32>(_batch.submits.size()),
            .queue                 = EQueueType::Graphics,
            .kind                  = EVulkanSubmissionBoundaryKind::PresentBridge,
            .dependency_wait_count = static_cast<uint32>(bridge.wait_events.size()),
        };
#endif
        auto recorded = graphics_queue.ReserveSubmission(std::move(bridge));
        graphics_queue.vk_device.GetCommandRecorder(EQueueType::Graphics).Record(recorded);
        VulkanSubmissionResult bridge_result;
        try {
            bridge_result = RunOnSubmissionThreadAndWait([&] {
                auto submit = [&] {
                    return SubmitRecordedSourceForRuntime(std::move(recorded));
                };
#if MOER_VULKAN_TEST_SUPPORT
                return ExecuteObservedSubmissionBoundary(boundary, submit);
#else
                return submit();
#endif
            });
        } catch (...) {
            ReportRequestFailure(ERequestKind::Submit, VulkanSubmissionDetail::EWorkerRequestFailurePhase::Process, std::current_exception());
            RejectRecordedSourceForRuntime(std::move(recorded), VK_ERROR_UNKNOWN, false);
            bridge_result.outcome = {EVulkanOperationStatus::Rejected, VK_ERROR_UNKNOWN};
        }
        if (!bridge_result.WasSubmitted()) {
            LOG_ERROR("[RHIExecutor][Vulkan] batch={} Present bridge failed status={} result={}", _batch.sequence, static_cast<uint32>(bridge_result.outcome.status), static_cast<int32>(bridge_result.outcome.result));
            const bool recoverable = bridge_result.IsRecoverableRejection();
            RejectRemainingSources(
                _batch,
                _execution,
                _batch.submits.size(),
                bridge_result.outcome.result,
                recoverable ? "Present bridge dependency rejection" : "Present bridge submission failure",
                recoverable,
                _first_unconsumed_source
            );
            return;
        }
        UpdateStreamFrontier(EQueueType::Graphics, *bridge_result.completion, true);
    }

    const auto present_result = RunOnSubmissionThreadAndWait([&] {
        auto submit = [&] {
            // Keep the request's receipt copy until Submission returns so the
            // outer exception barrier can resolve an interrupted request.
            return graphics_queue.PresentForRuntime(std::move(present.swapchain), present.source, present.receipt);
        };
#if MOER_VULKAN_TEST_SUPPORT
        const PresentReceiptRef receipt = present.receipt;
        return ExecuteObservedSubmissionBoundary(VulkanSubmissionBoundaryIdentity{
                                                     .batch_sequence  = _batch.sequence,
                                                     .operation_index = static_cast<uint32>(_batch.submits.size() + 1),
                                                     .queue           = EQueueType::Graphics,
                                                     .kind            = EVulkanSubmissionBoundaryKind::Present,
                                                 },
                                                 submit,
                                                 receipt);
#else
        return submit();
#endif
    });
    // Submission has terminalized or retained the receipt. Clear the request
    // copy before later bookkeeping can throw and try to reject it again.
    _batch.present.reset();
    if (present_result.WasSubmitted()) {
        UpdateStreamFrontier(EQueueType::Graphics, *present_result.completion, true);
    }
    if (present_result.IsHardFailure()) {
        LOG_ERROR("[RHIExecutor][Vulkan] batch={} Present failed status={} result={}", _batch.sequence, static_cast<uint32>(present_result.outcome.status), static_cast<int32>(present_result.outcome.result));
        LatchHardFailure({_batch.sequence, _batch.submits.size()}, static_cast<int32>(present_result.outcome.result));
    }
    // Retry/Recreate produces no GPU tail. Preserve the preceding stream tail,
    // including any bridge; Queue Sync still drains the CPU retirement marker.
}

void VulkanSubmissionExecutor::ProcessBatch(
    RHISubmissionBatch&              _batch,
    size_t&                          _first_unconsumed_source,
    std::shared_ptr<BatchExecution>& _execution_state
) {
    VulkanSegmentMaterializationResult materialization;
    if (!PrepareBatch(_batch, _first_unconsumed_source, materialization)) {
        return;
    }
    const bool pipeline_mode = CanPipelineBatch(_batch);
    if (pipeline_mode) {
        WaitForPipelineCapacity();
    } else {
        // Present, legacy/direct state inference, serial control, and a batch
        // crossing distinct logical wrappers of one native lane remain
        // explicit pipeline boundaries. Once the tail marker completes, this
        // coordinator may safely reuse the synchronous path and its existing
        // failure semantics.
        DrainPipelineBatches();
    }

    int32 latched_failure_result = static_cast<int32>(VK_ERROR_UNKNOWN);
    if (IsHardFailureAtOrBefore(
            SubmissionPosition{_batch.sequence, 0},
            &latched_failure_result
        )) {
        // Windowed admission can leave an already-accepted batch behind the
        // capacity item just retired above. Its Submission work must publish
        // terminal markers before this later batch is rejected directly.
        DrainPipelineBatches();
        RejectBatch(
            std::move(_batch),
            latched_failure_result,
            "submission runtime latched while waiting for pipeline admission",
            false,
            0,
            &_first_unconsumed_source
        );
        return;
    }

    // Retain failure observers before any command payload is moved.
    auto execution   = std::make_shared<BatchExecution>(_batch, pipeline_mode);
    _execution_state = execution;
#if MOER_VULKAN_TEST_SUPPORT
    for (size_t index = 0; index < execution->sources.size(); ++index) {
        auto&       source                   = execution->sources[index];
        const auto& metadata                 = materialization.sources[index];
        source.original_source_index         = metadata.original_source_index;
        source.source_segment_index          = metadata.source_segment_index;
        source.source_segment_count          = metadata.source_segment_count;
        source.cross_native_predecessor_wait = metadata.cross_native_predecessor_wait;
    }
#endif
    auto seal_on_exit = OnScopeExit([&]() noexcept {
        if (pipeline_mode) {
            execution->Seal();
        }
    });
    if (pipeline_mode) {
        pending_submission_batches.emplace_back(execution->submission_tasks_done->get_future());
        if (!logged_cross_batch_pipeline) {
            logged_cross_batch_pipeline = true;
            LOG_INFO("[RHIExecutor][Vulkan][SubmissionPipeline] mode=bounded-cross-batch batch_window={} per_native_lane_recorded_limit=1 ordered_submission=stable", batch_window);
        }
    }
    auto is_parallel_source = [&](size_t index) {
        return index < _batch.submits.size() &&
               _batch.topology.source_plans[index].segment_plan_count == 1 &&
               IsParallelTranslateCandidate(_batch.submits[index]);
    };
    for (size_t source_index = 0; source_index < _batch.submits.size(); ++source_index) {
        if (is_parallel_source(source_index)) {
            const uint64 scope     = _batch.submits[source_index].submit.async_queue_scope;
            size_t       group_end = source_index + 1;
            while (is_parallel_source(group_end) && _batch.submits[group_end].submit.async_queue_scope == scope) {
                ++group_end;
            }
            if (pipeline_mode || group_end - source_index > 1) {
                if (!ProcessParallelSources(_batch, execution, source_index, group_end, _first_unconsumed_source)) {
                    return;
                }
                source_index = group_end - 1;
                continue;
            }
        }
        if (!ProcessSerialSource(_batch, execution, source_index, _first_unconsumed_source)) {
            return;
        }
    }
    if (_batch.present) {
        ProcessPresent(_batch, execution, _first_unconsumed_source);
    }
}

void VulkanSubmissionExecutor::ProcessSync(ERHISyncDepth _depth) {
    RunOnSubmissionThreadAndWait([this, _depth] {
        LOG_INFO(
            "[RHIExecutor][Vulkan] sync begin depth={} copy_timeline={}",
            static_cast<uint32>(_depth),
            last_copy_timeline
        );
        // Runtime-side rejections deliberately own no GPU timeline.  Drain
        // every Completion queue by its independent CPU retirement serial so
        // Sync also observes callbacks/fence failure for those packets.
        RenderDevice::Get().GetCommandQueue(EQueueType::Graphics).Sync();
        RenderDevice::Get().GetCommandQueue(EQueueType::Compute).Sync();
        RenderDevice::Get().GetCopyQueue().Sync(last_copy_timeline);
        if (_depth == ERHISyncDepth::Present) {
            // Without per-Present fences, swapchain lifecycle boundaries
            // require a coarse idle wait after queue-local completion.
            RenderDevice::Get().WaitIdle();
        }
    });
    gpu_frontier.fill(std::nullopt);
    ResetStreamScope();
    last_copy_timeline = 0;
    LOG_INFO("[RHIExecutor][Vulkan] sync complete depth={}", static_cast<uint32>(_depth));
}

void VulkanSubmissionExecutor::CompleteRequest(std::optional<std::promise<void>>& completion, std::exception_ptr failure) {
    if (!completion) {
        return;
    }
    if (failure) {
        completion->set_exception(std::move(failure));
    } else {
        completion->set_value();
    }
    completion.reset();
}

} // namespace Moer::Render
