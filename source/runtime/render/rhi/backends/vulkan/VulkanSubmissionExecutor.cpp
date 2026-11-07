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

bool IsSerialControlSource(
    ERHITranslateExecutionClass _execution_class
) noexcept {
    return _execution_class ==
           ERHITranslateExecutionClass::SerialControl;
}

struct BatchPreflightResult {
    bool             valid{true};
    std::string_view reason{"none"};
    size_t           source_index{std::numeric_limits<size_t>::max()};
    size_t           command_index{std::numeric_limits<size_t>::max()};
};

BatchPreflightResult RejectPreflight(
    std::string_view _reason,
    size_t           _source_index = std::numeric_limits<size_t>::max(),
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
        const CmdSubmit& submit = entry.submit;
        const RHISourceSubmitPlan& source_plan = topology.source_plans[source_index];
        const bool submit_has_work = !submit.cmds.empty() || SubmitHasSideEffects(submit);

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
    const size_t configured_batch_window = batch_window;
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
                accepting        = false;
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
        completion.emplace();
    }
    sources.reserve(_batch.submits.size());
    const GpuCompletionPublishBatch common_completion_batch =
        GpuCompletionBackendAccess::BeginPublishBatch();
    const QueryPublishBatch common_query_batch =
        QueryBackendAccess::BeginPublishBatch();

    for (size_t source_index = 0;
         source_index < _batch.submits.size();
         ++source_index) {
        RHISubmissionRequest&   entry = _batch.submits[source_index];
        SubmissionSource        source{};
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

void VulkanSubmissionExecutor::BatchExecution::RejectFrom(
    size_t           _reject_from,
    int32            _result,
    bool             _recoverable,
    std::string_view _reason
) noexcept {
    const size_t begin = std::min(_reject_from, sources.size());
    const VkResult result =
        _result == static_cast<int32>(VK_SUCCESS) ?
            VK_ERROR_UNKNOWN :
            static_cast<VkResult>(_result);

    const std::string_view reason =
        _reason.empty() ?
            "Vulkan submission suffix was rejected before GPU completion" :
            _reason;

    uint64 publication_epoch = 0;
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
        completion->set_value();
    }
}

void VulkanSubmissionExecutor::BatchExecution::Seal() noexcept {
    if (work_state.Seal()) {
        completion->set_value();
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
    RejectFrom(first, result, recoverable, recoverable ? "Vulkan submission pipeline suffix was rejected by a dependency" : "Vulkan submission pipeline suffix was rejected after a hard failure");
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
            request.kind     = ERequestKind::Submit;
            request.batch    = std::move(_batch);
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
                .completion = std::move(reply),
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
                .completion = std::move(reply),
            });
            stop_completion = std::move(candidate);
            join_owner = true;
        }
        accepting = false;
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
                        &first_unconsumed_source,
                        execution
                    );
                },
                [&](const std::exception_ptr& _failure) {
                    // Stop must still release its waiter so ShutDown can join
                    // the owners. Sync reports failure for every requested depth.
                    CompleteRequest(
                        request.completion,
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
    while (in_flight_batches.size() >= batch_window) {
        MOER_PROFILE_SCOPE("Vulkan.WaitForPipelineCapacity");
        auto completion = std::move(in_flight_batches.front());
        in_flight_batches.pop_front();
        completion.get();
    }
}

void VulkanSubmissionExecutor::DrainPipelineBatches() {
    assert(
        GetCurrentRHIThreadRole() != ERHIThreadRole::Submission &&
        "Submission owner must not wait on its own pipeline work"
    );
    while (!in_flight_batches.empty()) {
        auto completion = std::move(in_flight_batches.front());
        in_flight_batches.pop_front();
        completion.get();
    }
}

void VulkanSubmissionExecutor::ResetStreamScope() noexcept {
    active_async_queue_scope = 0;
    async_scope_entry_frontier.fill(std::nullopt);
    async_scope_seen_queues.fill(false);
}

void VulkanSubmissionExecutor::PrepareStreamSubmit(
    CmdSubmit& _submit,
    EQueueType _queue
) {
    const RHIQueueTopology queue_topology =
        RenderDevice::Get().GetQueueTopology();
    auto same_native_queue = [&](EQueueType lhs, EQueueType rhs) {
        return queue_topology.Resolve(lhs).native_queue_id ==
               queue_topology.Resolve(rhs).native_queue_id;
    };
    auto append_unique_wait = [](CmdSubmit& submit, WaitEvent event) {
        const bool exists = std::any_of(
            submit.wait_events.begin(),
            submit.wait_events.end(),
            [&](const WaitEvent& candidate) {
                return candidate.timeline_handle == event.timeline_handle &&
                       candidate.value == event.value;
            }
        );
        if (!exists) {
            submit.wait_events.emplace_back(event);
        }
    };
    auto append_frontier_waits =
        [&](const auto& frontier) {
            for (size_t index = 0; index < frontier.size(); ++index) {
                if (!frontier[index].has_value()) {
                    continue;
                }
                const auto source_queue = static_cast<EQueueType>(index);
                if (same_native_queue(source_queue, _queue)) {
                    continue;
                }
                append_unique_wait(_submit, *frontier[index]);
            }
        };
    auto mark_scope_queue_seen = [&] {
        for (size_t index = 0; index < async_scope_seen_queues.size();
             ++index) {
            if (same_native_queue(static_cast<EQueueType>(index), _queue)) {
                async_scope_seen_queues[index] = true;
            }
        }
    };

    const uint64 async_scope = _submit.async_queue_scope;
    if (async_scope == 0) {
        ResetStreamScope();
        append_frontier_waits(gpu_frontier);
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
        append_frontier_waits(async_scope_entry_frontier);
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
    auto same_native_queue = [&](EQueueType lhs, EQueueType rhs) {
        return queue_topology.Resolve(lhs).native_queue_id ==
               queue_topology.Resolve(rhs).native_queue_id;
    };

    if (_collapse) {
        gpu_frontier.fill(std::nullopt);
    } else {
        for (size_t index = 0; index < gpu_frontier.size(); ++index) {
            if (same_native_queue(static_cast<EQueueType>(index), _queue)) {
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
            const bool recoverable = result.IsRecoverableRejection();
            _execution->RejectFrom(
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
    EQueueType                               _queue,
    std::optional<VulkanPendingSubmission>&& _packet,
    VkResult                                 _result,
    bool                                     _recoverable
) noexcept {
    if (!_packet) {
        return;
    }
    if (_result == VK_SUCCESS) {
        _result = VK_ERROR_UNKNOWN;
    }
    // Queue identity is part of the request, including reservation failures.
    const EQueueType packet_queue = _packet->context.queue_type;
    assert(packet_queue == _queue && "pending submission queue mismatch");
    if (packet_queue == EQueueType::Copy) {
        auto& queue = static_cast<VulkanCopyQueue&>(RenderDevice::Get().GetCopyQueue());
        queue.RejectRecordedForRuntime(std::move(*_packet), _result, _recoverable);
    } else {
        auto& queue = static_cast<VulkanCommandQueue&>(RenderDevice::Get().GetCommandQueue(packet_queue));
        queue.RejectRecordedForRuntime(std::move(*_packet), _result, _recoverable);
    }
}

void VulkanSubmissionExecutor::RejectSourceForRuntime(
    EQueueType _queue,
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
    auto reject_packet =
        [&](VkResult _result,
            bool _recoverable,
            std::string_view _reason) noexcept {
            if (_result == VK_SUCCESS) {
                _result = VK_ERROR_UNKNOWN;
            }
            {
                _batch->RejectFrom(
                    _source_index,
                    static_cast<int32>(_result),
                    _recoverable,
                    _reason
                );
            }
            if (slot.recorded.has_value()) {
                RejectRecordedSourceForRuntime(
                    slot.queue_type,
                    std::move(slot.recorded),
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
            _batch->RejectFrom(
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
        if (slot.queue_type == EQueueType::Copy) {
            last_copy_timeline = std::max(
                last_copy_timeline,
                submit_result.completion->value
            );
        }
        UpdateStreamFrontier(
            slot.queue_type,
            *submit_result.completion,
            slot.async_queue_scope == 0
        );
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
    ERequestKind                                        _kind,
    VulkanSubmissionDetail::EWorkerRequestFailurePhase _phase,
    const std::exception_ptr&                           _exception
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
    RHISubmissionBatch&&            _batch,
    int32                           _result,
    std::string_view                _reason,
    bool                            _recoverable,
    size_t                          _first_source,
    size_t*                         _next_unconsumed_source,
    std::shared_ptr<BatchExecution> _execution
) {
    const size_t first_source = std::min(_first_source, _batch.submits.size());
    const VkResult result =
        _result == static_cast<int32>(VK_SUCCESS) ?
            VK_ERROR_UNKNOWN :
            static_cast<VkResult>(_result);

    const GpuCompletionPublishBatch completion_batch =
        GpuCompletionBackendAccess::BeginPublishBatch();
    for (size_t source_index = first_source;
         source_index < _batch.submits.size();
         ++source_index) {
        CmdSubmit& submit = _batch.submits[source_index].submit;
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

void VulkanSubmissionExecutor::ProcessBatch(RHISubmissionBatch& _batch, size_t& _first_unconsumed_source, std::shared_ptr<BatchExecution>& _execution_state) {
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
        return;
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
        return;
    }

    const RHIQueueTopology                   queue_topology = RenderDevice::Get().GetQueueTopology();
    const VulkanSegmentMaterializationResult materialization =
        MaterializeMultiSegmentBatch(_batch, queue_topology);
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
            return;
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

    bool batch_crosses_aliased_native_lane = false;
    if (runtime_queues_share_native_lane) {
        for (size_t lhs = 0;
             lhs < _batch.submits.size() &&
             !batch_crosses_aliased_native_lane;
             ++lhs) {
            const EQueueType lhs_queue = _batch.submits[lhs].queue;
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
    const bool pipeline_mode =
        !_batch.present.has_value() && !_batch.submits.empty() && !batch_crosses_aliased_native_lane &&
        std::all_of(_batch.submits.begin(), _batch.submits.end(), [](const RHISubmissionRequest& entry) {
            return IsParallelTranslateCandidate(entry) && !entry.submit.b_sync && !entry.submit.b_tick_profiling &&
                   !entry.submit.b_delete_resources && !entry.submit.EmitsProfilingQueries();
        });

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

    // Capture strong, copyable terminal handles before any CmdSubmit moves
    // into a Translate worker. A pipeline failure can then publish every raw
    // or already-recorded suffix Future without waiting for per-queue
    // Completion packets that may sit behind a blocking callback.
    auto execution   = std::make_shared<BatchExecution>(_batch, pipeline_mode);
    _execution_state = execution;
    for (size_t index = 0; index < execution->sources.size(); ++index) {
        auto&       source                   = execution->sources[index];
        const auto& metadata                 = materialization.sources[index];
        source.original_source_index         = metadata.original_source_index;
        source.source_segment_index          = metadata.source_segment_index;
        source.source_segment_count          = metadata.source_segment_count;
        source.cross_native_predecessor_wait = metadata.cross_native_predecessor_wait;
    }
    auto seal_pipeline = [&]() noexcept {
        if (pipeline_mode) {
            execution->Seal();
        }
    };
    auto seal_on_exit = OnScopeExit(seal_pipeline);
    if (pipeline_mode) {
        in_flight_batches.emplace_back(execution->completion->get_future().share());
        if (!logged_cross_batch_pipeline) {
            logged_cross_batch_pipeline = true;
            LOG_INFO("[RHIExecutor][Vulkan][SubmissionPipeline] mode=bounded-cross-batch batch_window={} per_native_lane_recorded_limit=1 ordered_submission=stable", batch_window);
        }
    }

    auto seal_and_drain_pipeline = [&]() noexcept {
        if (!pipeline_mode) {
            return;
        }
        // The current state is itself present in in_flight_batches. Seal it
        // before waiting so an empty or partially handed-off failing batch
        // can publish its terminal completion instead of self-deadlocking.
        seal_pipeline();
        DrainPipelineBatches();
    };
    auto reject_remaining = [&](size_t _begin, VkResult _result, std::string_view _reason) {
        if (_result == VK_SUCCESS) {
            _result = VK_ERROR_UNKNOWN;
        }
        execution->RejectFrom(
            _begin, static_cast<int32>(_result), false, _reason
        );
        // prepare_source may have admitted a later ready native lane before
        // the stable submission cursor rejects an earlier source. Preserve
        // gpu_frontier for successfully submitted work, but force the next
        // batch (even one reusing the same graph scope id) to freeze a fresh
        // entry frontier and republish its first-lane waits.
        if (pipeline_mode) {
            execution->PublishFailure(
                _begin, static_cast<int32>(_result), false
            );
            seal_and_drain_pipeline();
        } else {
            ResetStreamScope();
        }
        LatchHardFailure(
            SubmissionPosition{_batch.sequence, _begin},
            static_cast<int32>(_result)
        );
        _first_unconsumed_source =
            std::min(_begin, _batch.submits.size());
        RejectBatch(
            std::move(_batch),
            static_cast<int32>(_result),
            _reason,
            false,
            _first_unconsumed_source,
            &_first_unconsumed_source,
            execution
        );
        _first_unconsumed_source = _batch.submits.size();
    };
    auto reject_remaining_recoverable =
        [&](size_t _begin, VkResult _result, std::string_view _reason) {
            if (_result == VK_SUCCESS) {
                _result = VK_ERROR_UNKNOWN;
            }
            execution->RejectFrom(
                _begin, static_cast<int32>(_result), true, _reason
            );
            if (pipeline_mode) {
                execution->PublishFailure(
                    _begin, static_cast<int32>(_result), true
                );
                seal_and_drain_pipeline();
            } else {
                ResetStreamScope();
            }
            _first_unconsumed_source =
                std::min(_begin, _batch.submits.size());
            RejectBatch(
                std::move(_batch),
                static_cast<int32>(_result),
                _reason,
                true,
                _first_unconsumed_source,
                &_first_unconsumed_source,
                execution
            );
            _first_unconsumed_source = _batch.submits.size();
        };

    auto same_native_queue = [&](EQueueType lhs, EQueueType rhs) {
        return queue_topology.Resolve(lhs).native_queue_id ==
               queue_topology.Resolve(rhs).native_queue_id;
    };
    auto append_unique_wait = [](CmdSubmit& submit, WaitEvent event) {
        const bool exists = std::any_of(
            submit.wait_events.begin(),
            submit.wait_events.end(),
            [&](const WaitEvent& candidate) {
                return candidate.timeline_handle == event.timeline_handle &&
                       candidate.value == event.value;
            }
        );
        if (!exists) {
            submit.wait_events.emplace_back(event);
        }
    };
    auto append_frontier_waits =
        [&](CmdSubmit& submit,
            EQueueType target_queue,
            const auto& frontier) {
            for (size_t index = 0; index < frontier.size(); ++index) {
                if (!frontier[index].has_value()) {
                    continue;
                }
                const auto source_queue = static_cast<EQueueType>(index);
                if (same_native_queue(source_queue, target_queue)) {
                    continue;
                }
                append_unique_wait(submit, *frontier[index]);
            }
        };
    auto has_foreign_same_native_frontier =
        [&](EQueueType target_queue, const auto& frontier) {
            for (size_t index = 0; index < frontier.size(); ++index) {
                if (!frontier[index].has_value()) {
                    continue;
                }
                const auto source_queue = static_cast<EQueueType>(index);
                if (source_queue != target_queue &&
                    same_native_queue(source_queue, target_queue)) {
                    return true;
                }
            }
            return false;
        };
    auto update_frontier =
        [&](EQueueType queue, WaitEvent completion, bool collapse) {
            UpdateStreamFrontier(queue, completion, collapse);
        };
    auto prepare_source = [&](size_t source_index) {
        RHISubmissionRequest&      entry = _batch.submits[source_index];
        const RHISourceSubmitPlan& source_plan =
            _batch.topology.source_plans[source_index];
        if (source_plan.segment_plan_count == 0) {
            return false;
        }

        assert(source_plan.segment_plan_count == 1);
        const RHISubmissionSegmentPlan& segment_plan =
            _batch.topology.segments[source_plan.segment_plan_begin];
        entry.submit.segments = {RHISubmitSegment{
            .queue = segment_plan.segment.queue,
            .begin = 0,
            .end   = entry.submit.cmds.size(),
        }};

        if (pipeline_mode) {
            return true;
        }
        PrepareStreamSubmit(entry.submit, entry.queue);
        return true;
    };
    auto is_parallel_translate_candidate = [&](size_t source_index) {
        return source_index < _batch.submits.size() &&
               _batch.topology.source_plans[source_index].segment_plan_count == 1 &&
               IsParallelTranslateCandidate(_batch.submits[source_index]);
    };
    auto process_parallel_translate_group = [&](size_t group_begin, size_t group_end) {
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
            reject_remaining(group_begin, VK_ERROR_UNKNOWN, "parallel Translate schedule rejected");
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
                seal_pipeline();
                DrainPipelineBatches();
            }
            size_t rejection_begin = group_end;
            for (const auto& slot : slots) {
                if (!slot.handed_off) {
                    rejection_begin = std::min(rejection_begin, slot.source_index);
                }
            }
            if (rejection_begin < group_end) {
                execution->RejectFrom(rejection_begin, static_cast<int32>(result), false, "parallel Translate group failed before Completion notification");
            }
            for (auto& slot : slots) {
                if (slot.handed_off) {
                    continue;
                }
                auto& entry = _batch.submits[slot.source_index];
                if (slot.recorded) {
                    RejectRecordedSourceForRuntime(slot.queue_type, std::move(slot.recorded), result, false);
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
        auto translate_slot =
            [&](SubmissionSource& slot, CmdSubmit&& submit) noexcept {
                const VulkanExecutableSourceMetadata& metadata =
                    materialization.sources[slot.source_index];
                const uint32 native_queue_id =
                    queue_topology.Resolve(
                        slot.queue_type
                    ).native_queue_id;
                slot.attempted = true;
#if MOER_VULKAN_TEST_SUPPORT
                NotifyVulkanSourceTranslation(
                    _batch.sequence,
                    static_cast<uint32>(slot.source_index),
                    metadata.original_source_index,
                    metadata.source_segment_index,
                    metadata.source_segment_count,
                    slot.queue_type,
                    native_queue_id,
                    slot.async_queue_scope,
                    EVulkanSourceTranslationPhase::Begin
                );
#endif
                slot.recorded =
                    TranslateSourceForRuntime(
                        slot.queue_type,
                        std::move(submit)
                    );
#if MOER_VULKAN_TEST_SUPPORT
                NotifyVulkanSourceTranslation(
                    _batch.sequence,
                    static_cast<uint32>(slot.source_index),
                    metadata.original_source_index,
                    metadata.source_segment_index,
                    metadata.source_segment_count,
                    slot.queue_type,
                    native_queue_id,
                    slot.async_queue_scope,
                    slot.recorded.has_value() ?
                        EVulkanSourceTranslationPhase::Recorded :
                        EVulkanSourceTranslationPhase::Failed
                );
#endif
            };
        auto fail_group = [&](size_t           failure_source,
                              VkResult         failure_result,
                              bool             recoverable_failure,
                              std::string_view failure_reason,
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
            execution->RejectFrom(
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
            if (recoverable_failure) {
                reject_remaining_recoverable(group_end, failure_result, failure_reason);
            } else {
                reject_remaining(group_end, failure_result, failure_reason);
            }
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
                if (!prepare_source(slot.source_index)) {
                    return fail_group(
                        slot.source_index,
                        VK_ERROR_UNKNOWN,
                        false,
                        "parallel Translate source lost its segment"
                    );
                }
                wave_slots.emplace_back(&slot);
            }

            bool dispatch_failed = false;
            if (wave_slots.size() > 1 && TaskGraph::IsInitialized()) {
                // LambdaTask::Dispatch can allocate before it queues a
                // task. Once it reaches TaskGraph::QueueTask, the engine's
                // lock-free path does not throw. A latch therefore gives
                // us a non-allocating, noexcept join: on a dispatch
                // exception, count down the current/unstarted suffix and
                // still wait for every already-queued worker.
                std::latch completed(static_cast<std::ptrdiff_t>(wave_slots.size()));
                size_t     dispatched_count = 0;
                for (; dispatched_count < wave_slots.size(); ++dispatched_count) {
                    SubmissionSource* slot = wave_slots[dispatched_count];
                    try {
                        RHISubmissionRequest* entry = &_batch.submits[slot->source_index];
                        (void)LambdaTask::Dispatch(
                            [slot, entry, &completed, &translate_slot] {
                                RHIThreadRoleScope translate_worker(ERHIThreadRole::Translate);
                                MOER_PROFILE_SCOPE("Vulkan.TranslateSubmission");
                                translate_slot(
                                    *slot, std::move(entry->submit)
                                );
                                completed.count_down();
                            },
                            EThread::AnyThread_NormalPri
                        );
                    } catch (...) {
                        dispatch_failed = true;
                        completed.count_down(static_cast<std::ptrdiff_t>(wave_slots.size() - dispatched_count)
                        );
                        break;
                    }
                }
                completed.wait();
            } else {
                for (SubmissionSource* slot : wave_slots) {
                    RHISubmissionRequest& entry = _batch.submits[slot->source_index];
                    translate_slot(
                        *slot, std::move(entry.submit)
                    );
                }
            }

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

                used_queues[static_cast<size_t>(_batch.submits[slot->source_index].queue)] = true;
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
                SubmissionSource&   slot  = slots[next_release_local];
                const auto          state = scheduler.GetState(slot.key);
                if (!state || *state != ETranslateWaveNodeState::Translated) {
                    break;
                }
                assert(slot.recorded.has_value());
                assert(!slot.handed_off);

                RHISubmissionRequest& entry = _batch.submits[slot.source_index];

                if (pipeline_mode) {
                    execution->AddWork();
                    bool handoff_succeeded = false;
                    try {
                        handoff_succeeded = EnqueueSubmissionWork(SubmissionWork(
                            [this, state = execution, source_index = slot.source_index] {
                                ExecutePipelineSource(state, source_index);
                            }
                        ));
                    } catch (...) {
                        ReportRequestFailure(
                            ERequestKind::Submit,
                            VulkanSubmissionDetail::EWorkerRequestFailurePhase::Process,
                            std::current_exception()
                        );
                    }
                    if (!handoff_succeeded) {
                        execution->FinishWork();
                        return fail_group(
                            slot.source_index,
                            VK_ERROR_UNKNOWN,
                            false,
                            "parallel Translate pipeline handoff failure"
                        );
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

                VulkanSubmissionResult submit_result{};
                try {
                    submit_result =
                        ExecuteOnSubmissionThread([this,
                                                   packet         = &slot.recorded,
                                                   batch_sequence = _batch.sequence,
                                                   source_index   = static_cast<uint32>(slot.source_index),
                                                   source_metadata =
                                                       materialization.sources[slot.source_index],
                                                   queue_type  = slot.queue_type,
                                                   async_scope = slot.async_queue_scope,
                                                   execution]() mutable {
                            VulkanSubmissionResult result =
                                SubmitRecordedSourceForRuntime(
                                    std::move(**packet), execution.get(), source_index
                                );
                            packet->reset();
#if MOER_VULKAN_TEST_SUPPORT
                            if (result.WasSubmitted()) {
                                NotifyVulkanSourceSubmitted(
                                    batch_sequence,
                                    source_index,
                                    source_metadata.original_source_index,
                                    source_metadata.source_segment_index,
                                    source_metadata.source_segment_count,
                                    queue_type,
                                    async_scope,
                                    source_metadata.cross_native_predecessor_wait
                                );
                            }
#endif
                            return result;
                        });
                } catch (...) {
                    ReportRequestFailure(
                        ERequestKind::Submit,
                        VulkanSubmissionDetail::EWorkerRequestFailurePhase::Process,
                        std::current_exception()
                    );
                    execution->RejectFrom(
                        slot.source_index,
                        static_cast<int32>(VK_ERROR_UNKNOWN),
                        false,
                        "parallel Submission handoff failed before Completion notification"
                    );
                    RejectRecordedSourceForRuntime(
                        slot.queue_type,
                        std::move(slot.recorded),
                        VK_ERROR_UNKNOWN,
                        false
                    );
                    slot.recorded.reset();
                    submit_result.outcome = {EVulkanOperationStatus::Rejected, VK_ERROR_UNKNOWN};
                }
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
                    assert(submit_result.completion.has_value());
                    if (entry.queue == EQueueType::Copy) {
                        last_copy_timeline = std::max(
                            last_copy_timeline,
                            submit_result.completion->value
                        );
                    }
                    update_frontier(entry.queue, *submit_result.completion, false);
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
    };

    // Keep every source in the request-owned batch until its queue call has
    // returned. If reorder, descriptor-lease, allocator, or native recording
    // setup throws, Run's request barrier can still terminalize the current
    // source callbacks/signals and every later source. Moving all sources into
    // a temporary expansion array here would lose those obligations on unwind.
    for (size_t source_index = 0; source_index < _batch.submits.size(); ++source_index) {
        if (is_parallel_translate_candidate(source_index)) {
            const uint64 async_scope = _batch.submits[source_index].submit.async_queue_scope;
            size_t       group_end   = source_index + 1;
            while (group_end < _batch.submits.size() && is_parallel_translate_candidate(group_end) &&
                   _batch.submits[group_end].submit.async_queue_scope == async_scope) {
                ++group_end;
            }
            if (pipeline_mode || group_end - source_index > 1) {
                if (!process_parallel_translate_group(source_index, group_end)) {
                    return;
                }
                source_index = group_end - 1;
                continue;
            }
        }

        RHISubmissionRequest& entry = _batch.submits[source_index];
        if (!prepare_source(source_index)) {
            _first_unconsumed_source = source_index + 1;
            continue;
        }

        const uint64 async_scope = entry.submit.async_queue_scope;
        const bool serial_control =
            IsSerialControlSource(
                entry.submit.translate_execution_class
            );
        if (entry.queue == EQueueType::Copy) {
            auto&                   copy_queue = static_cast<VulkanCopyQueue&>(RenderDevice::Get().GetCopyQueue());
            VulkanPendingSubmission recorded   = copy_queue.ReserveSubmission(std::move(entry.submit));
            copy_queue.device.GetCommandRecorder(EQueueType::Copy).Record(recorded);
            used_queues[static_cast<size_t>(EQueueType::Copy)] = true;

            VulkanSubmissionResult submit_result{};
            try {
                submit_result =
                    ExecuteOnSubmissionThread([this,
                                               packet          = &recorded,
                                               batch_sequence  = _batch.sequence,
                                               source_index    = static_cast<uint32>(source_index),
                                               source_metadata = materialization.sources[source_index],
                                               async_scope,
                                               serial_control,
                                               execution]() mutable {
                        auto submit =
                            [this,
                             packet,
                             &execution,
                             source_index]() mutable {
                                return SubmitRecordedSourceForRuntime(
                                    std::move(*packet), execution.get(), source_index
                                );
                            };
                        VulkanSubmissionResult result =
#if MOER_VULKAN_TEST_SUPPORT
                            serial_control ? ExecuteObservedSubmissionBoundary(
                                                 VulkanSubmissionBoundaryIdentity{
                                                     .batch_sequence  = batch_sequence,
                                                     .operation_index = source_index,
                                                     .queue           = EQueueType::Copy,
                                                     .kind            = EVulkanSubmissionBoundaryKind::SerialControl,
                                                 },
                                                 submit
                                             ) :
#endif
                                             submit();
#if MOER_VULKAN_TEST_SUPPORT
                        if (result.WasSubmitted()) {
                            NotifyVulkanSourceSubmitted(
                                batch_sequence,
                                source_index,
                                source_metadata.original_source_index,
                                source_metadata.source_segment_index,
                                source_metadata.source_segment_count,
                                EQueueType::Copy,
                                async_scope,
                                source_metadata.cross_native_predecessor_wait
                            );
                        }
#endif
                        return result;
                    });
            } catch (...) {
                ReportRequestFailure(
                    ERequestKind::Submit,
                    VulkanSubmissionDetail::EWorkerRequestFailurePhase::Process,
                    std::current_exception()
                );
                execution->RejectFrom(
                    source_index,
                    static_cast<int32>(VK_ERROR_UNKNOWN),
                    false,
                    "Copy Submission handoff failed before Completion notification"
                );
                copy_queue.RejectRecordedForRuntime(std::move(recorded), VK_ERROR_UNKNOWN, false);
                submit_result.outcome = {
                    EVulkanOperationStatus::Rejected,
                    VK_ERROR_UNKNOWN
                };
            }
            // SubmitRecordedSourceForRuntime has now either submitted or terminalized this
            // source. Never reject it again if later bookkeeping/logging throws.
            _first_unconsumed_source = source_index + 1;
            if (submit_result.WasSubmitted()) {
                assert(submit_result.completion.has_value());
                last_copy_timeline = std::max(
                    last_copy_timeline, submit_result.completion->value
                );
                update_frontier(
                    EQueueType::Copy,
                    *submit_result.completion,
                    async_scope == 0
                );
            } else if (submit_result.IsRecoverableRejection()) {
                // A failed prerequisite did not reach vkQueueSubmit. Drain the
                // current source on Completion, reject the rest of this
                // topology batch, and keep the runtime available for a later
                // independent batch.
                reject_remaining_recoverable(
                    source_index + 1,
                    submit_result.outcome.result,
                    "Copy submission dependency rejection"
                );
                return;
            } else if (submit_result.IsHardFailure()) {
                LOG_ERROR(
                    "[RHIExecutor][Vulkan] batch={} Copy submission failed status={} result={}",
                    _batch.sequence,
                    static_cast<uint32>(submit_result.outcome.status),
                    static_cast<int32>(submit_result.outcome.result)
                );
                // A failed native Copy submit still publishes a CPU retirement
                // marker. ProcessSync always enters Copy Sync, whose independent
                // retirement serial drains it without reading Copy-owner state.
                reject_remaining(
                    source_index + 1,
                    submit_result.outcome.result,
                    "Copy submission hard failure"
                );
                return;
            }
        } else {
            auto& queue = static_cast<VulkanCommandQueue&>(
                RenderDevice::Get().GetCommandQueue(entry.queue)
            );
            VulkanPendingSubmission recorded = queue.ReserveSubmission(std::move(entry.submit));
            queue.vk_device.GetCommandRecorder(entry.queue).Record(recorded);
            // Translation reserves a logical queue timeline and always
            // publishes a CPU completion marker, including rejected paths.
            used_queues[static_cast<size_t>(entry.queue)] = true;
            VulkanSubmissionResult submit_result{};
            try {
                submit_result = ExecuteOnSubmissionThread(
                    [this,
                     packet          = &recorded,
                     batch_sequence  = _batch.sequence,
                     source_index    = static_cast<uint32>(source_index),
                     source_metadata = materialization.sources[source_index],
                     queue_type      = entry.queue,
                     async_scope,
                     serial_control,
                     execution]() mutable {
                        auto submit =
                            [this,
                             packet,
                             &execution,
                             source_index]() mutable {
                                return SubmitRecordedSourceForRuntime(
                                    std::move(*packet), execution.get(), source_index
                                );
                            };
                        VulkanSubmissionResult result =
#if MOER_VULKAN_TEST_SUPPORT
                            serial_control ? ExecuteObservedSubmissionBoundary(
                                                 VulkanSubmissionBoundaryIdentity{
                                                     .batch_sequence  = batch_sequence,
                                                     .operation_index = source_index,
                                                     .queue           = queue_type,
                                                     .kind            = EVulkanSubmissionBoundaryKind::SerialControl,
                                                 },
                                                 submit
                                             ) :
#endif
                                             submit();
#if MOER_VULKAN_TEST_SUPPORT
                        if (result.WasSubmitted()) {
                            NotifyVulkanSourceSubmitted(
                                batch_sequence,
                                source_index,
                                source_metadata.original_source_index,
                                source_metadata.source_segment_index,
                                source_metadata.source_segment_count,
                                queue_type,
                                async_scope,
                                source_metadata.cross_native_predecessor_wait
                            );
                        }
#endif
                        return result;
                    }
                );
            } catch (...) {
                ReportRequestFailure(
                    ERequestKind::Submit,
                    VulkanSubmissionDetail::EWorkerRequestFailurePhase::Process,
                    std::current_exception()
                );
                execution->RejectFrom(
                    source_index,
                    static_cast<int32>(VK_ERROR_UNKNOWN),
                    false,
                    "command Submission handoff failed before Completion notification"
                );
                queue.RejectRecordedForRuntime(
                    std::move(recorded), VK_ERROR_UNKNOWN, false
                );
                submit_result.outcome = {
                    EVulkanOperationStatus::Rejected, VK_ERROR_UNKNOWN
                };
            }
            // SubmitRecordedSourceForRuntime owns the recorded packet through its
            // terminal result, including native rejection.
            _first_unconsumed_source = source_index + 1;
            if (submit_result.WasSubmitted()) {
                assert(submit_result.completion.has_value());
                update_frontier(
                    entry.queue,
                    *submit_result.completion,
                    async_scope == 0
                );
                used_queues[static_cast<size_t>(entry.queue)] = true;
            } else if (submit_result.IsRecoverableRejection()) {
                reject_remaining_recoverable(
                    source_index + 1,
                    submit_result.outcome.result,
                    "command submission dependency rejection"
                );
                return;
            } else if (submit_result.IsHardFailure()) {
                LOG_ERROR(
                    "[RHIExecutor][Vulkan] batch={} source_queue={} submission failed "
                    "status={} result={}",
                    _batch.sequence,
                    static_cast<uint32>(entry.queue),
                    static_cast<uint32>(submit_result.outcome.status),
                    static_cast<int32>(submit_result.outcome.result)
                );
                reject_remaining(
                    source_index + 1,
                    submit_result.outcome.result,
                    "command submission hard failure"
                );
                return;
            }
        }
    }

    if (_batch.present) {
        active_async_queue_scope = 0;
        async_scope_entry_frontier.fill(std::nullopt);
        async_scope_seen_queues.fill(false);
        RHIPresentRequest& present = *_batch.present;
        auto& graphics_queue = static_cast<VulkanCommandQueue&>(
            RenderDevice::Get().GetCommandQueue(EQueueType::Graphics)
        );
        CmdSubmit bridge = CommandList(EQueueType::Graphics).Submit();
        append_frontier_waits(bridge, EQueueType::Graphics, gpu_frontier);
        // A foreign logical queue sharing Graphics' native queue needs no GPU
        // semaphore wait, but it has an independent Completion owner. Emit an
        // empty Graphics marker so no-GPU-tail Retry/Recreate retirement stays
        // behind that predecessor instead of retiring resources immediately.
        const bool alias_completion_marker =
            has_foreign_same_native_frontier(
                EQueueType::Graphics,
                gpu_frontier
            );
        if (!bridge.wait_events.empty() || alias_completion_marker) {
            const uint32 bridge_dependency_wait_count =
                static_cast<uint32>(bridge.wait_events.size());
            VulkanPendingSubmission recorded = graphics_queue.ReserveSubmission(std::move(bridge));
            graphics_queue.vk_device.GetCommandRecorder(EQueueType::Graphics).Record(recorded);
            used_queues[static_cast<size_t>(EQueueType::Graphics)] = true;
            VulkanSubmissionResult bridge_result{};
            try {
                bridge_result = ExecuteOnSubmissionThread(
                    [
                        this,
                        packet = &recorded,
                        batch_sequence = _batch.sequence,
                        operation_index =
                            static_cast<uint32>(_batch.submits.size()),
                        bridge_dependency_wait_count
                    ]() mutable {
                        auto submit = [this, packet]() mutable {
                                return SubmitRecordedSourceForRuntime(
                                    std::move(*packet)
                                );
                            };
#if MOER_VULKAN_TEST_SUPPORT
                        return ExecuteObservedSubmissionBoundary(
                            VulkanSubmissionBoundaryIdentity{
                                .batch_sequence  = batch_sequence,
                                .operation_index = operation_index,
                                .queue            = EQueueType::Graphics,
                                .kind =
                                    EVulkanSubmissionBoundaryKind::
                                        PresentBridge,
                                .dependency_wait_count =
                                    bridge_dependency_wait_count,
                            }, submit
                        );
#else
                        return submit();
#endif
                    }
                );
            } catch (...) {
                ReportRequestFailure(
                    ERequestKind::Submit,
                    VulkanSubmissionDetail::EWorkerRequestFailurePhase::Process,
                    std::current_exception()
                );
                graphics_queue.RejectRecordedForRuntime(
                    std::move(recorded), VK_ERROR_UNKNOWN, false
                );
                bridge_result.outcome = {
                    EVulkanOperationStatus::Rejected, VK_ERROR_UNKNOWN
                };
            }
            if (!bridge_result.WasSubmitted()) {
                LOG_ERROR(
                    "[RHIExecutor][Vulkan] batch={} Present bridge failed status={} result={}",
                    _batch.sequence,
                    static_cast<uint32>(bridge_result.outcome.status),
                    static_cast<int32>(bridge_result.outcome.result)
                );
                if (bridge_result.IsRecoverableRejection()) {
                    reject_remaining_recoverable(
                        _batch.submits.size(),
                        bridge_result.outcome.result,
                        "Present bridge dependency rejection"
                    );
                } else {
                    reject_remaining(
                        _batch.submits.size(),
                        bridge_result.outcome.result,
                        "Present bridge submission failure"
                    );
                }
                return;
            }
            assert(bridge_result.completion.has_value());
            update_frontier(
                EQueueType::Graphics, *bridge_result.completion, true
            );
            used_queues[static_cast<size_t>(EQueueType::Graphics)] = true;
        }
        const VulkanSubmissionResult present_result =
            ExecuteOnSubmissionThread([&graphics_queue,
                                       present         = &present,
                                       batch_sequence  = _batch.sequence,
                                       operation_index = static_cast<uint32>(_batch.submits.size() + 1)]() mutable {
                const PresentReceiptRef receipt = present->receipt;
                auto submit = [&graphics_queue, present]() mutable {
                        return graphics_queue.PresentForRuntime(
                            std::move(present->swapchain),
                            present->source,
                            // Keep the request's receipt copy until the
                            // Submission owner returns so the exception
                            // barrier can resolve it.
                            present->receipt
                        );
                    };
#if MOER_VULKAN_TEST_SUPPORT
                return ExecuteObservedSubmissionBoundary(
                    VulkanSubmissionBoundaryIdentity{
                        .batch_sequence  = batch_sequence,
                        .operation_index = operation_index,
                        .queue            = EQueueType::Graphics,
                        .kind =
                            EVulkanSubmissionBoundaryKind::Present,
                    }, submit, receipt
                );
#else
                return submit();
#endif
            });
        // PresentForRuntime has now terminalized or retained the copied receipt.
        // Clear the request copy before any later diagnostic/bookkeeping can
        // throw, preserving an accepted/retry Present's existing semantics.
        _batch.present.reset();
        // Retry/Recreate still owns a logical timeline and completion marker;
        // Sync must drain that CPU-side retirement even though no GPU tail was
        // produced.
        used_queues[static_cast<size_t>(EQueueType::Graphics)] = true;
        if (present_result.WasSubmitted()) {
            assert(present_result.completion.has_value());
            update_frontier(
                EQueueType::Graphics, *present_result.completion, true
            );
            used_queues[static_cast<size_t>(EQueueType::Graphics)] = true;
        }
        if (present_result.IsHardFailure()) {
            LOG_ERROR(
                "[RHIExecutor][Vulkan] batch={} Present failed status={} result={}",
                _batch.sequence,
                static_cast<uint32>(present_result.outcome.status),
                static_cast<int32>(present_result.outcome.result)
            );
            LatchHardFailure(
                SubmissionPosition{_batch.sequence, _batch.submits.size()},
                static_cast<int32>(
                    present_result.outcome.result == VK_SUCCESS ?
                        VK_ERROR_UNKNOWN :
                        present_result.outcome.result
                )
            );
        }
        // Retry/Recreate means no copy submit happened.  Keep the previous
        // ordered tail (including a cross-queue bridge) as the stream tail.
    }
}

void VulkanSubmissionExecutor::ProcessSync(ERHISyncDepth _depth) {
    ExecuteOnSubmissionThread([this, _depth] {
        LOG_INFO(
            "[RHIExecutor][Vulkan] sync begin depth={} gfx={} compute={} copy_timeline={}",
            static_cast<uint32>(_depth),
            used_queues[static_cast<size_t>(EQueueType::Graphics)],
            used_queues[static_cast<size_t>(EQueueType::Compute)],
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
    async_scope_entry_frontier.fill(std::nullopt);
    async_scope_seen_queues.fill(false);
    active_async_queue_scope = 0;
    used_queues.fill(false);
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
