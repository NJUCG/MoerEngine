#include "VulkanQueue.h"
#include "VulkanDescriptor.h"
#include "VulkanDevice.h"
#include "VulkanQueueInternal.h"
#include "VulkanRHIResource.h"
#include "log/LogSystem.h"

#include <algorithm>
#include <exception>
#include <mutex>
#include <thread>

namespace Moer::Render {

void VkCommandQueue::Wait(WaitEvent) {
    LogQueueErrorNoexcept(
        "[RHIExecutor][Vulkan] direct Wait is unsupported; "
        "publish dependencies with the submission through RHIExecutor"
    );
}

VkCommandQueue::VkCommandQueue(VulkanDevice& _device, EQueueType _type) :
    CommandQueue(),
    vk_device(_device),
    queue(_type, _device),
    timestamp_pool(_device, VK_QUERY_TYPE_TIMESTAMP, s_queue_max_frame_in_flight * s_query_max_storage * 4, _type),
    profiler_storage(timestamp_pool) {
    timeline = MoerNew(VulkanTimeline(vk_device));

    try {
        completion_worker_running = true;
        completion_thread         = std::jthread(&VkCommandQueue::CompletionThreadMain, this);
    } catch (...) {
        {
            std::lock_guard lock(event_mutex);
            completion_worker_running = false;
        }
        queue_cv.notify_all();
        if (completion_thread.joinable()) {
            completion_thread.join();
        }

        MoerDelete(timeline);
        timeline = nullptr;
        throw;
    }
}

VkCommandQueue::~VkCommandQueue() {
    Sync();

    // Presentation can still own swapchain images after Copy completion.
    if (queue.GetType() == EQueueType::Graphics) {
        vk_device.WaitIdle();
    }

    {
        std::unique_lock<std::mutex> lock(event_mutex);
        completion_worker_running = false;
    }
    queue_cv.notify_all();
    completion_thread.join();

    Array<VulkanCommandContext*> recycled_command_contexts;
    command_contexts.PopAll(recycled_command_contexts);
    for (auto* command_context : recycled_command_contexts) {
        MoerDelete(command_context);
    }

    command_context_quarantine.clear();
    MoerDelete(timeline);
    vk_device.RecordQueueSyncComplete();
}

bool VkCommandQueue::ClaimRuntimeOwnership() {
    bool expected = false;
    if (executor_claimed.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel, std::memory_order_acquire
        )) {
        try {
            LOG_INFO("[RHIExecutor][Vulkan] queue={} ownership=Runtime", static_cast<uint32>(queue.GetType()));
        } catch (...) {
        }
        runtime_dependency_waits_enabled.store(true, std::memory_order_release);
        return true;
    }
    LogQueueErrorNoexcept(
        "[RHIExecutor][Vulkan] queue={} is already claimed by an executor", static_cast<uint32>(queue.GetType())
    );
    return false;
}

void VkCommandQueue::ReleaseRuntimeOwnership() noexcept {
    CancelRuntimeDependencyWaits();
    bool       expected = true;
    const bool released =
        executor_claimed.compare_exchange_strong(expected, false, std::memory_order_acq_rel, std::memory_order_acquire);
    assert(released && "only the owning Vulkan runtime may release a queue claim");
}

void VkCommandQueue::CancelRuntimeDependencyWaits() noexcept {
    runtime_dependency_waits_enabled.store(false, std::memory_order_release);
}

WaitEvent VkCommandQueue::Execute(CmdSubmit&& _submit) {
    LogQueueErrorNoexcept(
        "[RHIExecutor][Vulkan] direct Execute is unsupported; "
        "publish work through RHIExecutor"
    );
    RejectForRuntime(std::move(_submit), VK_ERROR_UNKNOWN, true);
    return {0, 0};
}

VkCommandQueue::PendingSubmission VkCommandQueue::TranslateForRuntime(CmdSubmit&& _submit) noexcept {
    assert(executor_claimed.load(std::memory_order_acquire));
    assert(GetCurrentRHIThreadRole() == ERHIThreadRole::Translate);

    auto              execution_lease = runtime_execution_gate.Acquire();
    PendingSubmission pending{
        std::move(_submit),
        VulkanOperationContext{
            .operation  = EVulkanFaultOperation::QueueSubmit,
            .queue_type = queue.GetType(),
            .queue      = queue.GetHandle(),
        },
        0
    };
    pending.execution_lease = std::move(execution_lease);
    try {
        std::lock_guard execution_lock(exec_mtx);
        pending.timeline            = last_reserved_timeline.fetch_add(1, std::memory_order_relaxed) + 1;
        pending.context.timeline    = pending.timeline;
        pending.context.work_serial = pending.timeline;
        RecordSubmissionCommands(pending);
    } catch (const std::exception& error) {
        RejectRecording(pending, {EVulkanOperationStatus::Faulted, VK_ERROR_UNKNOWN}, error.what());
    } catch (...) {
        RejectRecording(
            pending, {EVulkanOperationStatus::Faulted, VK_ERROR_UNKNOWN}, "unexpected command recording failure"
        );
    }
    // Both ready and rejected requests retain their owners until Submission
    // publishes the batch outcome and hands the packet to Completion.
    return pending;
}

void VkCommandQueue::RejectRecording(
    PendingSubmission&           _pending,
    const VulkanOperationResult& _outcome,
    std::string_view             _reason
) noexcept {
    LogQueueErrorNoexcept(
        "[VulkanQueue] recording rejected: timeline={} reason={} VkResult={}",
        _pending.timeline,
        _reason,
        static_cast<int32>(_outcome.result)
    );
    ResetSplitProfilingCpuFrame();
    _pending.submission_outcome    = _outcome;
    _pending.recoverable_rejection = _outcome.status == EVulkanOperationStatus::Rejected && !vk_device.IsFaulted() &&
                                     _outcome.result != VK_ERROR_DEVICE_LOST;
    vk_device.RecordRejectedSubmit();
}

VulkanRuntimeSubmissionResult VkCommandQueue::SubmitRecordedForRuntime(
    PendingSubmission                     _recorded,
    const VulkanRuntimePreCompletionHook* _pre_completion
) noexcept {
    assert(
        GetCurrentRHIThreadRole() == ERHIThreadRole::Submission &&
        "runtime native submit must run on the Submission owner"
    );
    assert(_recorded.execution_lease.OwnsGate() && "runtime recorded submit must retain queue execution ownership");
    const uint64 submitted_timeline = _recorded.timeline;
    const bool   split_profiling_frame =
        _recorded.submit.EmitsProfilingQueries() && _recorded.submit.ProfilingPhase() != ERHIProfilingPhase::Complete;

    if (!_recorded.submission_outcome.has_value()) {
        try {
            std::unique_lock<std::mutex> execution_lock(exec_mtx);
            const VulkanOperationResult  submit_result = SubmitRecorded(_recorded, &runtime_dependency_waits_enabled);
            if (split_profiling_frame && !submit_result.Succeeded()) {
                ResetSplitProfilingCpuFrame();
            }
        } catch (const std::exception& error) {
            LogQueueErrorNoexcept(
                "[RHIExecutor][Vulkan] recorded submission threw before retirement: {}", error.what()
            );
        } catch (...) {
            LogQueueErrorNoexcept("[RHIExecutor][Vulkan] recorded submission threw before retirement");
        }
    }

    if (!_recorded.submission_outcome.has_value()) {
        queue.DiscardPendingSubmitState();
        vk_device.RecordRejectedSubmit();
        _recorded.submission_outcome    = MakeUnsubmittedOutcome(VK_ERROR_UNKNOWN, false);
        _recorded.recoverable_rejection = false;
    }

    VulkanRuntimeSubmissionResult runtime_result{
        .outcome               = *_recorded.submission_outcome,
        .recoverable_rejection = _recorded.recoverable_rejection,
    };
    if (_recorded.submission_outcome->Succeeded()) {
        runtime_result.completion = WaitEvent{uint64(timeline), submitted_timeline};
    }
    if (_pre_completion != nullptr && _pre_completion->callback != nullptr) {
        _pre_completion->callback(_pre_completion->context, runtime_result);
    }
    if (!_recorded.completion_enqueued) {
        CommitRuntimeSubmitCompletion(_recorded, *_recorded.submission_outcome);
    }
    if (split_profiling_frame && !_recorded.submission_outcome->Succeeded()) {
        ResetSplitProfilingCpuFrame();
    }
    return runtime_result;
}

void VkCommandQueue::RejectRecordedForRuntime(
    PendingSubmission&& _recorded,
    VkResult            _result,
    bool                _recoverable
) noexcept {
    assert(
        _recorded.execution_lease.OwnsGate() && "a failed Translate -> Submission handoff must retain queue ownership"
    );
    ResetSplitProfilingCpuFrame();
    if (!_recorded.submission_outcome.has_value()) {
        vk_device.RecordRejectedSubmit();
        _recorded.submission_outcome    = MakeUnsubmittedOutcome(_result, _recoverable);
        _recorded.recoverable_rejection = _recoverable;
    }
    if (!_recorded.completion_enqueued) {
        CommitRuntimeSubmitCompletion(_recorded, *_recorded.submission_outcome);
    }
}

void VkCommandQueue::RejectForRuntime(
    CmdSubmit&&                 _submit,
    VkResult                    _result,
    bool                        _recoverable,
    VulkanBatchCompletionTicket _batch_completion
) noexcept {
    // This request never acquired recording ownership. Reject only its own
    // work; an active Translate/Submission packet may still own profiling state.
    const VulkanOperationResult outcome = MakeUnsubmittedOutcome(_result, _recoverable);
    vk_device.RecordRejectedSubmit();

    try {
        const std::string_view          rejection_reason = "Vulkan runtime rejected the submit before GPU completion";
        const GpuCompletionPublishBatch completion_batch = _submit.gpu_completion_publish_batch.Valid() ?
                                                               _submit.gpu_completion_publish_batch :
                                                               GpuCompletionBackendAccess::BeginPublishBatch();
        _submit.PublishPendingGpuCompletionErrors(rejection_reason, completion_batch);
        PublishUnsubmittedQueryErrors(_submit, nullptr, rejection_reason);
        Array<SignalEvent> rejected_signals           = std::move(_submit.signal_events);
        Array<FenceRef>    rejected_signal_keepalives = std::move(_submit.signal_rejection_keepalives);
        _submit.signal_events.clear();
        _submit.signal_rejection_keepalives.clear();
        TerminalizeUnsubmittedSignals(vk_device, rejected_signals, outcome);
        std::unique_lock<std::mutex>                      lock(event_mutex);
        const uint64                                      retirement_serial = retirement_enqueued_serial + 1;
        const std::shared_ptr<VulkanBatchCompletionGroup> completion_group =
            _batch_completion.Valid() ? _batch_completion.group : nullptr;
        event_queue.emplace_back(
            0,
            retirement_serial,
            outcome,
            VulkanOperationContext{
                .operation  = EVulkanFaultOperation::QueueSubmit,
                .queue_type = queue.GetType(),
                .queue      = queue.GetHandle(),
            },
            false,
            false,
            std::move(_submit),
            VulkanCommandContextBatch{},
            VulkanDescriptorBufferLease{},
            Array<RHIResource*>{},
            std::move(_batch_completion)
        );
        if (completion_group) {
            std::erase_if(batch_completion_settlements, [](const VulkanBatchCompletionSettlement& settlement) {
                return settlement.group.expired();
            });
            batch_completion_settlements.emplace_back(
                VulkanBatchCompletionSettlement{
                    .retirement_serial = retirement_serial,
                    .group             = completion_group,
                }
            );
        }
        retirement_enqueued_serial = retirement_serial;
        (void)rejected_signal_keepalives;
    } catch (...) {
        // Continuing would destroy callbacks/signals outside Completion and
        // violate the ownership contract. Allocation failure is therefore
        // treated as an unrecoverable runtime bookkeeping failure.
        std::terminate();
    }
    // CompleteAll shares this CV; notify_one could leave the worker asleep.
    queue_cv.notify_all();
}

VulkanOperationResult
VkCommandQueue::SubmitRecorded(PendingSubmission& _pending, const std::atomic_bool* _continue_waiting) {
    assert(
        !_pending.submission_outcome.has_value() && !_pending.completion_enqueued &&
        "a terminal submission cannot be replayed"
    );
    const auto reject = [&](const VulkanOperationResult& outcome, std::string_view reason) {
        LogQueueErrorNoexcept(
            "[VulkanQueue] submission rejected: timeline={} reason={} VkResult={}",
            _pending.timeline,
            reason,
            static_cast<int32>(outcome.result)
        );
        vk_device.RecordRejectedSubmit();
        _pending.submission_outcome    = outcome;
        _pending.recoverable_rejection = outcome.status == EVulkanOperationStatus::Rejected && !vk_device.IsFaulted() &&
                                         outcome.result != VK_ERROR_DEVICE_LOST;
        return outcome;
    };

    if (!WaitForSubmittedDependencies(vk_device, _pending.submit.wait_events, queue.GetType(), _continue_waiting)) {
        return reject(
            {EVulkanOperationStatus::Rejected, GetRejectedSubmitResult(vk_device)},
            "submission dependency was rejected or cancelled"
        );
    }

    VulkanPresentationSourceStateTransaction presentation_transaction;
    try {
        presentation_transaction = EvaluatePresentationSourceStateProgram(_pending.presentation_source_program);
    } catch (const std::logic_error& error) {
        return reject({EVulkanOperationStatus::Rejected, VK_ERROR_FEATURE_NOT_PRESENT}, error.what());
    } catch (const std::bad_alloc& error) {
        return reject({EVulkanOperationStatus::Rejected, VK_ERROR_OUT_OF_HOST_MEMORY}, error.what());
    } catch (const std::exception& error) {
        return reject({EVulkanOperationStatus::Faulted, VK_ERROR_UNKNOWN}, error.what());
    } catch (...) {
        return reject(
            {EVulkanOperationStatus::Faulted, VK_ERROR_UNKNOWN}, "presentation-state transaction evaluation failed"
        );
    }

    constexpr VkPipelineStageFlags2 completion_stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    queue.Signal(timeline, _pending.timeline, completion_stage);
    for (const WaitEvent& event : _pending.submit.wait_events) {
        queue.Wait(reinterpret_cast<VulkanTimeline*>(event.timeline_handle), event.value, completion_stage);
    }
    for (const SignalEvent& event : _pending.submit.signal_events) {
        queue.Signal(reinterpret_cast<VulkanTimeline*>(event.timeline_handle), event.value, completion_stage);
    }
    for (const VkSemaphoreSubmitInfo& wait : _pending.native_waits) {
        queue.Wait(wait.semaphore, wait.stageMask);
    }
    for (const VkSemaphoreSubmitInfo& signal : _pending.native_signals) {
        queue.Signal(signal.semaphore, signal.stageMask);
    }
    const VulkanOperationResult outcome = _pending.ordered_cmd_lists.empty() ?
                                              queue.SubmitEmpty(_pending.context) :
                                              queue.Submit(_pending.ordered_cmd_lists, _pending.context);
    // Once accepted, later bookkeeping failure must still retire this packet
    // as GPU-submitted. Save the decision before publishing observable state.
    _pending.submission_outcome = outcome;
    if (outcome.Succeeded()) {
        CommitAcceptedPresentationSourceStateTransaction(presentation_transaction);
        MarkSubmissionAccepted(timeline, _pending.timeline, _pending.submit.signal_events);
    }
    return outcome;
}

void VkCommandQueue::CommitRuntimeSubmitCompletion(
    PendingSubmission&           _recorded,
    const VulkanOperationResult& _outcome
) noexcept {
    assert(!_recorded.completion_enqueued);
    try {
        if (!_outcome.Succeeded()) {
            _recorded.command_contexts.abandoned.reserve(
                _recorded.command_contexts.abandoned.size() + _recorded.command_contexts.submitted.size()
            );
            const std::string_view          rejection_reason = "Vulkan submission was rejected before GPU completion";
            const GpuCompletionPublishBatch completion_batch = _recorded.submit.gpu_completion_publish_batch.Valid() ?
                                                                   _recorded.submit.gpu_completion_publish_batch :
                                                                   GpuCompletionBackendAccess::BeginPublishBatch();
            _recorded.submit.PublishPendingGpuCompletionErrors(rejection_reason, completion_batch);
            PublishUnsubmittedQueryErrors(_recorded.submit, &_recorded.command_contexts, rejection_reason);
            TerminalizeUnsubmittedSignal(vk_device, timeline, _recorded.timeline, _outcome);
            Array<SignalEvent> rejected_signals           = std::move(_recorded.submit.signal_events);
            Array<FenceRef>    rejected_signal_keepalives = std::move(_recorded.submit.signal_rejection_keepalives);
            _recorded.submit.signal_events.clear();
            _recorded.submit.signal_rejection_keepalives.clear();
            TerminalizeUnsubmittedSignals(vk_device, rejected_signals, _outcome);
            for (auto& command_context : _recorded.command_contexts.submitted) {
                _recorded.command_contexts.abandoned.emplace_back(std::move(command_context));
            }
            _recorded.command_contexts.submitted.clear();
            (void)rejected_signal_keepalives;
        }

        std::unique_lock<std::mutex>                      lock(event_mutex);
        const uint64                                      retirement_serial = retirement_enqueued_serial + 1;
        const std::shared_ptr<VulkanBatchCompletionGroup> completion_group =
            _recorded.batch_completion.Valid() ? _recorded.batch_completion.group : nullptr;
        event_queue.emplace_back(
            _recorded.timeline,
            retirement_serial,
            _outcome,
            _recorded.context,
            _outcome.Succeeded(),
            _outcome.Succeeded(),
            std::move(_recorded.submit),
            std::move(_recorded.command_contexts),
            std::move(_recorded.descriptor_lease),
            std::move(_recorded.deferred_releases),
            std::move(_recorded.batch_completion)
        );
        if (completion_group) {
            std::erase_if(batch_completion_settlements, [](const VulkanBatchCompletionSettlement& settlement) {
                return settlement.group.expired();
            });
            batch_completion_settlements.emplace_back(
                VulkanBatchCompletionSettlement{
                    .retirement_serial = retirement_serial,
                    .group             = completion_group,
                }
            );
        }
        retirement_enqueued_serial    = retirement_serial;
        _recorded.completion_enqueued = true;
    } catch (...) {
        // The packet may own command pools already submitted to the GPU. It is
        // never safe to unwind and destroy it on the Submission owner.
        std::terminate();
    }
    // CompleteAll shares this CV; notify_one could leave the worker asleep.
    queue_cv.notify_all();
}

void VkCommandQueue::Sync() {
    if (GetCurrentRHIThreadRole() == ERHIThreadRole::Completion) {
        LogQueueErrorNoexcept(
            "[RHIExecutor][Vulkan] queue={} Completion owner cannot block "
            "on a retirement queue",
            static_cast<uint32>(queue.GetType())
        );
        return;
    }
    uint64 target_timeline = 0;
    {
        std::unique_lock<std::mutex> lock(exec_mtx);
        target_timeline = last_reserved_timeline.load(std::memory_order_relaxed);
    }

    CompleteAll(target_timeline);

    if (vk_device.IsFaulted()) {
        return;
    }

    Array<RHIResource*> deleted_resources;
    if (queue.GetType() != EQueueType::Graphics) {
        return;
    }
    {
        std::unique_lock<std::mutex> lock(exec_mtx);
        if (last_reserved_timeline.load(std::memory_order_relaxed) == target_timeline) {
            vk_device.deferred_release_queue.PopAll(deleted_resources);
        }
    }
    for (auto* resource : deleted_resources) {
        MoerDelete(resource);
    }
}

UniquePtr<VulkanCommandContext> VkCommandQueue::AcquireCommandContext() {
    // Completion publishes only successfully completed and reset command contexts
    // into this pool. Pool membership is therefore the reuse gate; if
    // Completion is delayed by unrelated CPU callbacks, allocate overflow
    // storage instead of stalling the Translate owner.
    UniquePtr<VulkanCommandContext> command_context(command_contexts.Pop());
    if (command_context) {
        return command_context;
    }
    return MakeUnique<VulkanCommandContext>(&vk_device, queue.GetType());
}

Array<RHIResource*> VkCommandQueue::TakeDeferredReleases() {
    if (queue.GetType() != EQueueType::Graphics) {
        return {};
    }

    Array<RHIResource*> deleted_resources;
    // Recorded and submitted commands retain their resources through
    // RHICommandLifetime until Completion finishes all callback tiers.
    vk_device.deferred_release_queue.PopAll(deleted_resources);

    return deleted_resources;
}

} // namespace Moer::Render
