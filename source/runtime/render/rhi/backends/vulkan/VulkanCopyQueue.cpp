#include "VulkanCommandRecorder.h"
#include "VulkanDevice.h"
#include "VulkanQueue.h"
#include "VulkanQueueInternal.h"
#include "log/LogSystem.h"
#include "platform/Platform.h"
#include "profile/ProfileScope.h"
#include "rhi/RHIThreadOwnership.h"
#include "rhi/executor/RHIExecutorBackend.h"
#if MOER_VULKAN_TEST_SUPPORT
#include "vulkan/VulkanSubmissionTestHooksInternal.h"
#endif

#include <algorithm>
#include <thread>

namespace Moer::Render {

VulkanCopyQueue::VulkanCopyQueue(VulkanDevice& _device) :
    CopyQueue(),
    device(_device),
    queue(EQueueType::Copy, _device) {
    timeline = MoerNew(VulkanTimeline)(_device);
    enabled  = true;
    try {
        thread = std::jthread([this]() {
            ExecuteThread();
        });
    } catch (...) {
        {
            std::lock_guard lock(event_mutex);
            enabled.store(false, std::memory_order_release);
        }
        queue_cv.notify_all();
        if (thread.joinable()) {
            thread.join();
        }
        timeline = nullptr;
        throw;
    }
}

VulkanCopyQueue::~VulkanCopyQueue() {
    CancelRuntimeDependencyWaits();
    CompleteAll(last_frame.load(std::memory_order_acquire));
    {
        std::lock_guard lock(event_mutex);
        enabled.store(false, std::memory_order_release);
    }
    queue_cv.notify_all();
    if (thread.joinable()) {
        thread.join();
    }
    timeline = nullptr;
    device.RecordQueueSyncComplete();
}

void VulkanCopyQueue::EnableRuntimeDependencyWaits() noexcept {
    dependency_waits_enabled.store(true, std::memory_order_release);
}

void VulkanCopyQueue::CancelRuntimeDependencyWaits() noexcept {
    dependency_waits_enabled.store(false, std::memory_order_release);
}

bool VulkanCopyQueue::ClaimRuntimeOwnership() {
    bool expected = false;
    if (executor_claimed.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel,
            std::memory_order_acquire
        )) {
        EnableRuntimeDependencyWaits();
        return true;
    }
    try {
        LOG_ERROR(
            "[RHIExecutor][Vulkan] Copy queue is already claimed by an executor"
        );
    } catch (...) {
    }
    return false;
}

void VulkanCopyQueue::ReleaseRuntimeOwnership() noexcept {
    CancelRuntimeDependencyWaits();
    bool expected = true;
    const bool released = executor_claimed.compare_exchange_strong(
        expected,
        false,
        std::memory_order_acq_rel,
        std::memory_order_acquire
    );
    assert(released && "only the owning Vulkan runtime may release Copy");
}

WaitEvent VulkanCopyQueue::Execute(CmdSubmit&& _evt) {
    try {
        LOG_ERROR(
            "[RHIExecutor][Vulkan] direct Copy Execute is unsupported; "
            "publish work through RHIExecutor"
        );
    } catch (...) {
    }
    RejectForRuntime(std::move(_evt), VK_ERROR_UNKNOWN, true);
    return {0, 0};
}

VulkanPendingSubmission VulkanCopyQueue::ReserveSubmission(CmdSubmit&& _submit) noexcept {
    assert(executor_claimed.load(std::memory_order_acquire));
    assert(GetCurrentRHIThreadRole() == ERHIThreadRole::Translate);
    auto                    execution_lease = runtime_execution_gate.Acquire();
    VulkanPendingSubmission recorded{
        std::move(_submit),
        VulkanOperationContext{
            .operation  = EVulkanFaultOperation::QueueSubmit,
            .queue_type = EQueueType::Copy,
            .queue      = queue.GetHandle(),
        }
    };
    recorded.execution_lease = std::move(execution_lease);
    try {
        const bool has_queue_work = !recorded.submit.cmds.empty() || !recorded.submit.wait_events.empty() ||
                                    !recorded.submit.signal_events.empty() || !recorded.submit.callbacks.empty() ||
                                    !recorded.submit.success_callbacks.empty() ||
                                    !recorded.submit.gpu_completion_tokens.empty() ||
                                    !recorded.submit.query_tokens.empty() || recorded.submit.b_sync ||
                                    recorded.submit.b_tick_profiling || recorded.submit.b_delete_resources;
        if (!has_queue_work) {
            recorded.submission_outcome  = VulkanOperationResult{};
            recorded.completion_enqueued = true;
            return recorded;
        }

        std::unique_lock<std::mutex> execution_lock(exec_mutex);
        recorded.timeline = last_frame.fetch_add(1, std::memory_order_relaxed) + 1;
        recorded.context  = VulkanOperationContext{
             .operation   = EVulkanFaultOperation::QueueSubmit,
             .queue_type  = EQueueType::Copy,
             .queue       = queue.GetHandle(),
             .timeline    = recorded.timeline,
             .work_serial = recorded.timeline,
        };
    } catch (...) {
        device.RecordRejectedSubmit();
        recorded.submission_outcome = VulkanOperationResult{EVulkanOperationStatus::Faulted, VK_ERROR_UNKNOWN};
        LogQueueErrorNoexcept("[VulkanCopyQueue] failed to reserve a submission timeline");
    }
    return recorded;
}

VulkanSubmissionResult VulkanCopyQueue::SubmitRecordedForRuntime(VulkanPendingSubmission& _recorded) noexcept {
    assert(
        executor_claimed.load(std::memory_order_acquire) &&
        "runtime Copy submit requires an exclusively claimed queue"
    );
    assert(
        GetCurrentRHIThreadRole() == ERHIThreadRole::Submission &&
        "runtime Copy submit must run on the Submission owner"
    );
    assert(
        _recorded.execution_lease.OwnsGate() &&
        "runtime Copy packet must retain queue execution ownership"
    );

    if (_recorded.timeline == 0 &&
        _recorded.completion_enqueued) {
        return {
            .outcome = *_recorded.submission_outcome,
        };
    }

    assert(_recorded.context.queue_type == EQueueType::Copy);
    if (!_recorded.submission_outcome.has_value()) {
        try {
            std::unique_lock<std::mutex> execution_lock(exec_mutex);
            if (!WaitForSubmittedDependencies(
                    device,
                    _recorded.submit.wait_events,
                    EQueueType::Copy,
                    &dependency_waits_enabled
                )) {
                device.RecordRejectedSubmit();
                _recorded.submission_outcome = VulkanOperationResult{
                    EVulkanOperationStatus::Rejected,
                    GetRejectedSubmitResult(device)
                };
                _recorded.recoverable_rejection =
                    !device.IsFaulted();
            } else {
                std::optional<
                    VulkanResourceStateValidator>
                           resource_validation{};
                const auto reject_resource_validation =
                    [&](const VulkanOperationResult& _outcome,
                        std::string_view             _reason) {
                        try {
                            LOG_ERROR(
                                "[RHIExecutor][Vulkan] Copy rejected "
                                "before native submit: timeline={} "
                                "reason={} VkResult={}",
                                _recorded.timeline,
                                _reason,
                                static_cast<int32>(_outcome.result)
                            );
                        } catch (...) {
                        }
                        device.RecordRejectedSubmit();
                        _recorded.submission_outcome = _outcome;
                        _recorded.recoverable_rejection =
                            _outcome.status ==
                                EVulkanOperationStatus::Rejected &&
                            !device.IsFaulted() &&
                            _outcome.result !=
                                VK_ERROR_DEVICE_LOST;
                    };
                try {
                    resource_validation.emplace(
                        device, EQueueType::Copy, _recorded.resource_state_program, &_recorded.submit, timeline.Get(), _recorded.timeline
                    );
                } catch (const std::logic_error& error) {
                    reject_resource_validation(
                        VulkanOperationResult{
                            EVulkanOperationStatus::Rejected,
                            VK_ERROR_FEATURE_NOT_PRESENT
                        },
                        error.what()
                    );
                } catch (const std::bad_alloc& error) {
                    reject_resource_validation(
                        VulkanOperationResult{
                            EVulkanOperationStatus::Rejected,
                            VK_ERROR_OUT_OF_HOST_MEMORY
                        },
                        error.what()
                    );
                } catch (const std::exception& error) {
                    reject_resource_validation(
                        VulkanOperationResult{
                            EVulkanOperationStatus::Faulted,
                            VK_ERROR_UNKNOWN
                        },
                        error.what()
                    );
                } catch (...) {
                    reject_resource_validation(
                        VulkanOperationResult{
                            EVulkanOperationStatus::Faulted,
                            VK_ERROR_UNKNOWN
                        },
                        "resource-state contract validation "
                        "failed"
                    );
                }

                if (!_recorded.submission_outcome.has_value()) {
                    queue.Signal(
                        timeline,
                        _recorded.timeline,
                        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT
                    );
                    for (auto& event :
                         _recorded.submit.wait_events) {
                        queue.Wait(
                            reinterpret_cast<VulkanTimeline*>(
                                event.timeline_handle
                            ),
                            event.value
                        );
                    }
                    for (auto& event :
                         _recorded.submit.signal_events) {
                        queue.Signal(
                            reinterpret_cast<VulkanTimeline*>(
                                event.timeline_handle
                            ),
                            event.value,
                            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT
                        );
                    }

                    assert(_recorded.command_context && "Copy Translate must retain its command context");
                    auto& vk_command_context = *_recorded.command_context;
                    _recorded.submission_outcome =
                        queue.Submit(vk_command_context.GetCommandList(), _recorded.context);
                    if (_recorded.submission_outcome->Succeeded()) {
                        resource_validation->Commit();
                        MarkSubmissionAccepted(
                            timeline.Get(),
                            _recorded.timeline,
                            _recorded.submit.signal_events
                        );
                    }
                }
            }
        } catch (const std::exception& error) {
            try {
                LOG_ERROR(
                    "[RHIExecutor][Vulkan] Copy recorded submit threw before retirement: {}",
                    error.what()
                );
            } catch (...) {
            }
        } catch (...) {
            try {
                LOG_ERROR(
                    "[RHIExecutor][Vulkan] Copy recorded submit threw before retirement"
                );
            } catch (...) {
            }
        }
    }

    if (!_recorded.submission_outcome.has_value()) {
        queue.DiscardPendingSubmitState();
        device.RecordRejectedSubmit();
        _recorded.submission_outcome =
            MakeUnsubmittedOutcome(VK_ERROR_UNKNOWN, false);
        _recorded.recoverable_rejection = false;
    }
    VulkanSubmissionResult runtime_result{
        .outcome               = *_recorded.submission_outcome,
        .recoverable_rejection = _recorded.recoverable_rejection,
    };
    if (_recorded.submission_outcome->Succeeded()) {
        runtime_result.completion =
            WaitEvent{
                uint64(timeline.Get()),
                _recorded.timeline
            };
    }
    return runtime_result;
}

void VulkanCopyQueue::RejectRecordedForRuntime(
    VulkanPendingSubmission&& _recorded,
    VkResult                  _result,
    bool                      _recoverable
) noexcept {
    assert(
        _recorded.execution_lease.OwnsGate() &&
        "a failed Copy Translate -> Submission handoff must retain ownership"
    );
    if (!_recorded.submission_outcome.has_value()) {
        queue.DiscardPendingSubmitState();
        device.RecordRejectedSubmit();
        _recorded.submission_outcome =
            MakeUnsubmittedOutcome(_result, _recoverable);
        _recorded.recoverable_rejection = _recoverable;
    }
    if (!_recorded.completion_enqueued) {
        CommitRuntimeSubmitCompletion(
            _recorded, *_recorded.submission_outcome
        );
    }
}
void VulkanCopyQueue::RejectForRuntime(
    CmdSubmit&& _submit,
    VkResult    _result,
    bool        _recoverable
) noexcept {
    device.RecordRejectedSubmit();
    VulkanPendingSubmission rejected{std::move(_submit)};
    rejected.context = VulkanOperationContext{
        .operation  = EVulkanFaultOperation::QueueSubmit,
        .queue_type = EQueueType::Copy,
        .queue      = queue.GetHandle(),
    };
    rejected.submission_outcome =
        MakeUnsubmittedOutcome(_result, _recoverable);
    rejected.recoverable_rejection = _recoverable;
    CommitRuntimeSubmitCompletion(
        rejected, *rejected.submission_outcome
    );
}

void VulkanCopyQueue::CommitRuntimeSubmitCompletion(
    VulkanPendingSubmission&     _recorded,
    const VulkanOperationResult& _outcome
) noexcept {
    assert(!_recorded.completion_enqueued);
    try {
        if (!_outcome.Succeeded()) {
            const std::string_view rejection_reason =
                "Vulkan Copy submission was rejected before GPU completion";
            const GpuCompletionPublishBatch completion_batch =
                _recorded.submit.gpu_completion_publish_batch.Valid() ?
                    _recorded.submit.gpu_completion_publish_batch :
                    GpuCompletionBackendAccess::BeginPublishBatch();
            _recorded.submit.PublishPendingGpuCompletionErrors(
                rejection_reason, completion_batch
            );
            PublishUnsubmittedQueryErrors(_recorded.submit, _recorded.command_context.get(), rejection_reason);
            if (_recorded.timeline != 0) {
                TerminalizeUnsubmittedSignal(
                    device,
                    timeline.Get(),
                    _recorded.timeline,
                    _outcome
                );
            }
            Array<SignalEvent> rejected_signals =
                std::move(_recorded.submit.signal_events);
            Array<FenceRef> rejected_signal_keepalives =
                std::move(
                    _recorded.submit.signal_rejection_keepalives
                );
            _recorded.submit.signal_events.clear();
            _recorded.submit.signal_rejection_keepalives.clear();
            TerminalizeUnsubmittedSignals(
                device, rejected_signals, _outcome
            );
            (void)rejected_signal_keepalives;
        }
        std::unique_lock<std::mutex> lock(event_mutex);
        const uint64                 retirement_serial = retirement_enqueued_serial + 1;
        event_queue.emplace_back(
            _recorded.timeline,
            retirement_serial,
            _outcome,
            _recorded.context,
            _outcome.Succeeded(),
            _recorded.timeline != 0,
            std::move(_recorded.submit),
            std::move(_recorded.command_context),
            VulkanDescriptorBufferLease{},
            Array<RHIResource*>{}
        );
        retirement_enqueued_serial    = retirement_serial;
        _recorded.completion_enqueued = true;
    } catch (...) {
        std::terminate();
    }
    queue_cv.notify_one();
}

void VulkanCopyQueue::Sync(uint64 _timeline) {
    if (GetCurrentRHIThreadRole() == ERHIThreadRole::Completion) {
        try {
            LOG_ERROR(
                "[RHIExecutor][Vulkan] Copy Completion owner cannot block on "
                "a retirement queue"
            );
        } catch (...) {
        }
        return;
    }
    CompleteAll(_timeline);
}

FenceRef VulkanCopyQueue::GetFenceHandle() {
    return FenceRef(timeline);
}

void VulkanCopyQueue::ExecuteThread() {
    Platform::SetCurrentThreadName("Vulkan Copy Completion");
    RHIThreadRoleScope owner_scope(ERHIThreadRole::Completion);
    while (true) {
        std::optional<VulkanSubmissionCompletion> evt;
        {
            std::unique_lock<std::mutex> lock(event_mutex);
            queue_cv.wait(lock, [this]() {
                return !enabled.load(std::memory_order_acquire) || !event_queue.empty();
            });
            if (!enabled.load(std::memory_order_acquire) && event_queue.empty()) {
                break;
            }
            evt.emplace(std::move(event_queue.front()));
            event_queue.pop_front();
        }

        MOER_PROFILE_SCOPE("Vulkan.ProcessCopyCompletion");
        const uint64 event_timeline    = evt->timeline;
        const uint64 retirement_serial = evt->retirement_serial;
        evt->Complete(device, *timeline);

        {
            // Pair predicate updates with CompleteAll's check-and-wait lock.
            std::lock_guard lock(event_mutex);
            uint64 settled = cpu_settled_frame.load(std::memory_order_relaxed);
            while (settled < event_timeline &&
                   !cpu_settled_frame.compare_exchange_weak(
                       settled,
                       event_timeline,
                       std::memory_order_release,
                       std::memory_order_relaxed
                   )) {
            }
            uint64 settled_serial =
                retirement_settled_serial.load(std::memory_order_relaxed);
            while (settled_serial < retirement_serial &&
                   !retirement_settled_serial.compare_exchange_weak(
                       settled_serial,
                       retirement_serial,
                       std::memory_order_release,
                       std::memory_order_relaxed
                   )) {}
        }
        settled_cv.notify_all();
    }
}

void VulkanCopyQueue::CompleteAll(uint64 _timeline) {
    std::unique_lock lock(event_mutex);
    const uint64     target_retirement_serial = retirement_enqueued_serial;
    settled_cv.wait(lock, [this, _timeline, target_retirement_serial] {
        return cpu_settled_frame.load(std::memory_order_acquire) >= _timeline &&
               retirement_settled_serial.load(std::memory_order_acquire) >= target_retirement_serial;
    });
}

} // namespace Moer::Render
