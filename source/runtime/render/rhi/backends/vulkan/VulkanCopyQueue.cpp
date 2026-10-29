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

VulkanRecordedCopySubmission VulkanCopyQueue::ReserveSubmission(CmdSubmit&& _submit) noexcept {
    assert(executor_claimed.load(std::memory_order_acquire));
    assert(GetCurrentRHIThreadRole() == ERHIThreadRole::Translate);
    auto execution_lease = runtime_execution_gate.Acquire();
    VulkanRecordedCopySubmission recorded{std::move(_submit)};
    recorded.execution_lease = std::move(execution_lease);
    try {
        const bool has_queue_work = !recorded.submit.cmds.empty() || !recorded.submit.wait_events.empty() ||
                                    !recorded.submit.signal_events.empty() || !recorded.submit.callbacks.empty() ||
                                    !recorded.submit.success_callbacks.empty() ||
                                    !recorded.submit.gpu_completion_tokens.empty() ||
                                    !recorded.submit.query_tokens.empty() || recorded.submit.b_sync ||
                                    recorded.submit.b_tick_profiling || recorded.submit.b_delete_resources;
        if (!has_queue_work) {
            recorded.retirement_outcome     = {};
            recorded.native_submit_resolved = true;
            recorded.completion_committed   = true;
            return recorded;
        }

        std::unique_lock<std::mutex> execution_lock(exec_mutex);
        recorded.logical_timeline = last_frame.fetch_add(1, std::memory_order_relaxed) + 1;
        recorded.context = VulkanOperationContext{
            .operation   = EVulkanFaultOperation::QueueSubmit,
            .queue_type  = EQueueType::Copy,
            .queue       = queue.GetHandle(),
            .timeline    = recorded.logical_timeline,
            .work_serial = recorded.logical_timeline,
        };
    } catch (...) {
        device.RecordRejectedSubmit();
        recorded.retirement_outcome     = {EVulkanOperationStatus::Faulted, VK_ERROR_UNKNOWN};
        recorded.native_submit_resolved = true;
        LogQueueErrorNoexcept("[VulkanCopyQueue] failed to reserve a submission timeline");
    }
    return recorded;
}

VulkanSubmissionResult VulkanCopyQueue::SubmitRecordedForRuntime(
    VulkanRecordedCopySubmission          _recorded,
    const VulkanRuntimePreCompletionHook* _pre_completion
) noexcept {
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

    if (_recorded.logical_timeline == 0 &&
        _recorded.completion_committed) {
        return {
            .outcome = _recorded.retirement_outcome,
        };
    }

    if (!_recorded.native_submit_resolved) {
        try {
            std::unique_lock<std::mutex> execution_lock(exec_mutex);
            if (!WaitForSubmittedDependencies(
                    device,
                    _recorded.submit.wait_events,
                    EQueueType::Copy,
                    &dependency_waits_enabled
                )) {
                device.RecordRejectedSubmit();
                _recorded.retirement_outcome = {
                    EVulkanOperationStatus::Rejected,
                    GetRejectedSubmitResult(device)
                };
                _recorded.recoverable_rejection =
                    !device.IsFaulted();
                _recorded.native_submit_resolved = true;
            } else {
                std::optional<
                    VulkanPresentationSourceStateTransaction>
                    presentation_transaction{};
                const auto reject_presentation_transaction =
                    [&](const VulkanOperationResult& _outcome,
                        std::string_view             _reason) {
                        try {
                            LOG_ERROR(
                                "[RHIExecutor][Vulkan] Copy rejected "
                                "before native submit: timeline={} "
                                "reason={} VkResult={}",
                                _recorded.logical_timeline,
                                _reason,
                                static_cast<int32>(_outcome.result)
                            );
                        } catch (...) {
                        }
                        device.RecordRejectedSubmit();
                        _recorded.retirement_outcome = _outcome;
                        _recorded.recoverable_rejection =
                            _outcome.status ==
                                EVulkanOperationStatus::Rejected &&
                            !device.IsFaulted() &&
                            _outcome.result !=
                                VK_ERROR_DEVICE_LOST;
                        _recorded.native_submit_resolved = true;
                    };
                try {
                    presentation_transaction.emplace(
                        EvaluatePresentationSourceStateProgram(
                            _recorded.presentation_source_program
                        )
                    );
                } catch (const std::logic_error& error) {
                    reject_presentation_transaction(
                        VulkanOperationResult{
                            EVulkanOperationStatus::Rejected,
                            VK_ERROR_FEATURE_NOT_PRESENT
                        },
                        error.what()
                    );
                } catch (const std::bad_alloc& error) {
                    reject_presentation_transaction(
                        VulkanOperationResult{
                            EVulkanOperationStatus::Rejected,
                            VK_ERROR_OUT_OF_HOST_MEMORY
                        },
                        error.what()
                    );
                } catch (const std::exception& error) {
                    reject_presentation_transaction(
                        VulkanOperationResult{
                            EVulkanOperationStatus::Faulted,
                            VK_ERROR_UNKNOWN
                        },
                        error.what()
                    );
                } catch (...) {
                    reject_presentation_transaction(
                        VulkanOperationResult{
                            EVulkanOperationStatus::Faulted,
                            VK_ERROR_UNKNOWN
                        },
                        "presentation-state transaction evaluation "
                        "failed"
                    );
                }

                if (!_recorded.native_submit_resolved) {
                    queue.Signal(
                        timeline,
                        _recorded.logical_timeline,
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

                    assert(
                        _recorded.command_contexts.submitted.size() == 1 &&
                        "Copy Translate must publish exactly one "
                        "command context"
                    );
                    auto& vk_command_context = *_recorded.command_contexts.submitted.front();
                    _recorded.retirement_outcome =
                        queue.Submit(vk_command_context.GetCommandList(), _recorded.context);
                    _recorded.native_submit_resolved = true;
                    if (_recorded.retirement_outcome.Succeeded()) {
                        CommitAcceptedPresentationSourceStateTransaction(
                            *presentation_transaction
                        );
                        MarkSubmissionAccepted(
                            timeline.Get(),
                            _recorded.logical_timeline,
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

    if (!_recorded.native_submit_resolved) {
        queue.DiscardPendingSubmitState();
        device.RecordRejectedSubmit();
        _recorded.retirement_outcome =
            MakeUnsubmittedOutcome(VK_ERROR_UNKNOWN, false);
        _recorded.recoverable_rejection = false;
        _recorded.native_submit_resolved = true;
    }
    VulkanSubmissionResult runtime_result{
        .outcome               = _recorded.retirement_outcome,
        .recoverable_rejection = _recorded.recoverable_rejection,
    };
    if (_recorded.retirement_outcome.Succeeded()) {
        runtime_result.completion =
            WaitEvent{
                uint64(timeline.Get()),
                _recorded.logical_timeline
            };
    }
    if (_pre_completion != nullptr &&
        _pre_completion->callback != nullptr) {
        _pre_completion->callback(
            _pre_completion->context, runtime_result
        );
    }
    if (!_recorded.completion_committed) {
        CommitRuntimeSubmitCompletion(
            _recorded, _recorded.retirement_outcome
        );
    }
    return runtime_result;
}

void VulkanCopyQueue::RejectRecordedForRuntime(
    VulkanRecordedCopySubmission&& _recorded,
    VkResult                       _result,
    bool                           _recoverable
) noexcept {
    assert(
        _recorded.execution_lease.OwnsGate() &&
        "a failed Copy Translate -> Submission handoff must retain ownership"
    );
    if (!_recorded.native_submit_resolved) {
        queue.DiscardPendingSubmitState();
        device.RecordRejectedSubmit();
        _recorded.retirement_outcome =
            MakeUnsubmittedOutcome(_result, _recoverable);
        _recorded.recoverable_rejection = _recoverable;
        _recorded.native_submit_resolved = true;
    }
    if (!_recorded.completion_committed) {
        CommitRuntimeSubmitCompletion(
            _recorded, _recorded.retirement_outcome
        );
    }
}
void VulkanCopyQueue::RejectForRuntime(
    CmdSubmit&& _submit,
    VkResult    _result,
    bool        _recoverable,
    VulkanBatchCompletionTicket _batch_completion
) noexcept {
    device.RecordRejectedSubmit();
    VulkanRecordedCopySubmission rejected{std::move(_submit)};
    rejected.batch_completion = std::move(_batch_completion);
    rejected.context = VulkanOperationContext{
        .operation  = EVulkanFaultOperation::QueueSubmit,
        .queue_type = EQueueType::Copy,
        .queue      = queue.GetHandle(),
    };
    rejected.retirement_outcome =
        MakeUnsubmittedOutcome(_result, _recoverable);
    rejected.recoverable_rejection = _recoverable;
    rejected.native_submit_resolved = true;
    CommitRuntimeSubmitCompletion(
        rejected, rejected.retirement_outcome
    );
}

void VulkanCopyQueue::CommitRuntimeSubmitCompletion(
    VulkanRecordedCopySubmission& _recorded,
    const VulkanOperationResult&  _outcome
) noexcept {
    assert(!_recorded.completion_committed);
    try {
        if (!_outcome.Succeeded()) {
            _recorded.command_contexts.abandoned.reserve(
                _recorded.command_contexts.abandoned.size() + _recorded.command_contexts.submitted.size()
            );
            const std::string_view rejection_reason =
                "Vulkan Copy submission was rejected before GPU completion";
            const GpuCompletionPublishBatch completion_batch =
                _recorded.submit.gpu_completion_publish_batch.Valid() ?
                    _recorded.submit.gpu_completion_publish_batch :
                    GpuCompletionBackendAccess::BeginPublishBatch();
            _recorded.submit.PublishPendingGpuCompletionErrors(
                rejection_reason, completion_batch
            );
            PublishUnsubmittedQueryErrors(_recorded.submit, &_recorded.command_contexts, rejection_reason);
            if (_recorded.logical_timeline != 0) {
                TerminalizeUnsubmittedSignal(
                    device,
                    timeline.Get(),
                    _recorded.logical_timeline,
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
            for (auto& command_context : _recorded.command_contexts.submitted) {
                _recorded.command_contexts.abandoned.emplace_back(std::move(command_context));
            }
            _recorded.command_contexts.submitted.clear();
            (void)rejected_signal_keepalives;
        }
        std::unique_lock<std::mutex> lock(event_mutex);
        const uint64 retirement_serial = retirement_enqueued_serial + 1;
        const std::shared_ptr<VulkanBatchCompletionGroup>
            completion_group = _recorded.batch_completion.Valid() ?
                _recorded.batch_completion.group :
                nullptr;
        event_queue.emplace_back(
            _recorded.logical_timeline,
            retirement_serial,
            _outcome,
            _recorded.context,
            _outcome.Succeeded(),
            _recorded.logical_timeline != 0,
            std::move(_recorded.submit),
            std::move(_recorded.command_contexts),
            VulkanDescriptorBufferLease{},
            Array<RHIResource*>{},
            std::move(_recorded.batch_completion)
        );
        if (completion_group) {
            std::erase_if(
                batch_completion_settlements,
                [](const VulkanBatchCompletionSettlement& settlement) {
                    return settlement.group.expired();
                }
            );
            batch_completion_settlements.emplace_back(
                VulkanBatchCompletionSettlement{
                    .retirement_serial = retirement_serial,
                    .group             = completion_group,
                }
            );
        }
        retirement_enqueued_serial = retirement_serial;
        _recorded.completion_committed = true;
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
        std::optional<CompletionEvent> evt;
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
        const uint64 event_timeline = evt->timeline;
        auto& batch = evt->batch;
        bool batch_gpu_success = false;
        VkResult completion_result = batch.outcome.result;
        if (completion_result == VK_SUCCESS && !batch.gpu_submitted) {
            completion_result = VK_ERROR_UNKNOWN;
        }
        const bool recoverable_rejection =
            !batch.gpu_submitted &&
            batch.outcome.status ==
                EVulkanOperationStatus::Rejected &&
            !device.IsFaulted() &&
            batch.outcome.result != VK_ERROR_DEVICE_LOST;

        if (batch.gpu_submitted) {
            if (timeline->Wait(event_timeline) && !device.IsFaulted()) {
                batch_gpu_success = true;
                completion_result = VK_SUCCESS;

                for (auto& command_context : batch.command_contexts.submitted) {
                    const VkResult query_result =
                        command_context->PrepareQueriesAfterGpuCompletion(batch.context);
                    if (query_result != VK_SUCCESS) {
                        completion_result = query_result;
                        batch_gpu_success = false;
                        break;
                    }
                }
                if (batch_gpu_success && device.IsFaulted()) {
                    completion_result =
                        device.GetFirstFaultResult();
                    batch_gpu_success = false;
                }

                if (!batch_gpu_success && batch.owns_queue_timeline) {
                    timeline->Fail(completion_result);
                }
            } else {
                // Wait failed or the device faulted; read the recorded error.
                completion_result = timeline->QueryGpuCompletion(event_timeline);
                if (batch.owns_queue_timeline) {
                    timeline->Fail(completion_result);
                }
            }
        } else if (batch.owns_queue_timeline) {
            if (recoverable_rejection) {
                timeline->Reject(event_timeline);
            } else {
                timeline->Fail(completion_result);
            }
        }

        for (const SignalEvent& event : batch.submit.signal_events) {
            auto* fence =
                reinterpret_cast<VulkanTimeline*>(event.timeline_handle);
            if (fence == nullptr) {
                continue;
            }
            if (batch_gpu_success) {
                fence->RecordGpuCompletion(event.value);
            } else if (recoverable_rejection) {
                fence->Reject(event.value);
            } else {
                fence->Fail(completion_result);
            }
        }

        const QueryPublishBatch query_batch =
            batch.submit.query_publish_batch.Valid() ?
                batch.submit.query_publish_batch :
                QueryBackendAccess::BeginPublishBatch();
        const std::string_view query_failure_reason =
            batch_gpu_success ?
                "Vulkan query did not resolve after GPU completion" :
                "Vulkan submission did not reach successful GPU completion";
        for (auto& command_context : batch.command_contexts.submitted) {
            command_context->PublishQueriesAfterGpuCompletion(
                query_batch, batch_gpu_success, query_failure_reason
            );
        }
        for (auto& command_context : batch.command_contexts.abandoned) {
            command_context->PublishQueriesAfterGpuCompletion(
                query_batch,
                false,
                "Vulkan Copy query recorder was abandoned before native submission"
            );
        }
        batch.submit.PublishPendingQueryErrors(
            query_failure_reason, query_batch
        );

        const GpuCompletionPublishBatch
            gpu_completion_batch =
                batch.submit.
                        gpu_completion_publish_batch.Valid() ?
                    batch.submit.
                        gpu_completion_publish_batch :
                    GpuCompletionBackendAccess::
                        BeginPublishBatch();
        batch.submit.PublishPendingGpuCompletionOutcome(
            batch_gpu_success,
            "Vulkan Copy submission did not reach successful GPU completion",
            gpu_completion_batch
        );

        bool recycle_batch =
            batch_gpu_success && !device.IsFaulted();
        if (recycle_batch) {
            for (auto& command_context : batch.command_contexts.submitted) {
                recycle_batch = command_context->CompleteSuccessCallbacks() && recycle_batch;
            }
        }

        if (batch.descriptor_lease) {
            // A failed completion wait does not prove the GPU stopped
            // reading. Keep those ranges unavailable until heap teardown.
            if (!batch.gpu_submitted || batch_gpu_success) {
                device.GetGlobalDescriptorHeap().RecycleDescriptorBufferLease(
                    std::move(batch.descriptor_lease)
                );
            }
            batch.descriptor_lease.reset();
        }

        if (!batch.gpu_submitted) {
            FinalizeRejectedBindlessUpdates(batch.submit);
        }
        Array<std::function<void()>> callbacks{};
        Array<std::function<void()>> success_callbacks{};
        std::optional<
            VulkanBatchCompletionGroup::Participant>
            grouped_completion{};
        if (batch.batch_completion.Valid()) {
            grouped_completion.emplace();
            grouped_completion->lifetime = std::move(batch.submit.lifetime);
            grouped_completion->gpu_completion_tokens =
                std::move(
                    batch.submit.gpu_completion_tokens
                );
            grouped_completion->gpu_completion_batch =
                gpu_completion_batch;
            grouped_completion->query_tokens =
                std::move(batch.submit.query_tokens);
            grouped_completion->query_batch = query_batch;
            grouped_completion->callbacks =
                std::move(batch.submit.callbacks);
            grouped_completion->success_callbacks =
                std::move(batch.submit.success_callbacks);
            grouped_completion->deferred_releases =
                std::move(batch.deferred_releases);
            grouped_completion->device       = &device;
            grouped_completion->gpu_success  =
                batch_gpu_success;
            grouped_completion->release_safe = false;
            batch.submit.gpu_completion_publish_batch =
                {};
            batch.submit.query_publish_batch = {};
        } else {
            callbacks =
                std::move(batch.submit.callbacks);
            success_callbacks =
                std::move(batch.submit.success_callbacks);
        }
        batch.submit.cmds.clear();
        batch.submit.cached_args.clear();
        batch.submit.segments.clear();
        batch.submit.wait_events.clear();
        batch.submit.signal_events.clear();
        batch.submit.debug_label.clear();

        if (!grouped_completion.has_value()) {
            batch.submit.NotifyPendingGpuCompletions(
                gpu_completion_batch
            );
            batch.submit.NotifyPendingQueries(query_batch);
            for (auto& command_context : batch.command_contexts.submitted) {
                command_context->NotifyQueriesAfterGpuCompletion(query_batch);
            }
            for (auto& command_context : batch.command_contexts.abandoned) {
                command_context->NotifyQueriesAfterGpuCompletion(query_batch);
            }
            batch.submit.query_tokens.clear();
        }

        device.GetCommandRecorder(EQueueType::Copy).RecycleContexts(batch.command_contexts, recycle_batch);

        if (grouped_completion.has_value()) {
            VulkanBatchCompletionTicket ticket =
                std::move(batch.batch_completion);
            ticket.group->Arrive(
                ticket.participant_index,
                std::move(*grouped_completion)
            );
        } else {
            InvokeCallbacksNoexcept(
                callbacks, "[VulkanCopyQueue] runtime completion"
            );
            if (batch_gpu_success) {
                InvokeCallbacksNoexcept(
                    success_callbacks,
                    "[VulkanCopyQueue] runtime success completion"
                );
            }

            for (auto* resource : batch.deferred_releases) {
                if (batch_gpu_success) {
                    MoerDelete(resource);
                } else {
                    device.EnqueueDeferredRelease(resource);
                }
            }
        }
        batch.deferred_releases.clear();
        callbacks.clear();
        success_callbacks.clear();
        batch.submit.lifetime.reset();

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
                   )) {}
            const uint64 retirement_serial = evt->retirement_serial;
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
    Array<std::shared_ptr<VulkanBatchCompletionGroup>>
        completion_groups{};
    uint64 target_retirement_serial = 0;
    {
        std::unique_lock<std::mutex> lock(event_mutex);
        target_retirement_serial = retirement_enqueued_serial;
        for (const VulkanBatchCompletionSettlement& settlement :
             batch_completion_settlements) {
            if (settlement.retirement_serial >
                target_retirement_serial) {
                continue;
            }
            if (auto group = settlement.group.lock()) {
                completion_groups.emplace_back(std::move(group));
            }
        }
#if MOER_VULKAN_TEST_SUPPORT
        NotifyVulkanQueueLocalSyncWait(
            EQueueType::Copy,
            target_retirement_serial,
            completion_groups.size()
        );
#endif
        settled_cv.wait(
            lock,
            [this, _timeline, target_retirement_serial]() {
                return cpu_settled_frame.load(
                           std::memory_order_acquire
                       ) >= _timeline &&
                       retirement_settled_serial.load(
                           std::memory_order_acquire
                       ) >= target_retirement_serial;
            }
        );
    }

    for (const auto& group : completion_groups) {
        group->WaitUntilSettled();
    }

    std::unique_lock<std::mutex> lock(event_mutex);
    std::erase_if(
        batch_completion_settlements,
        [target_retirement_serial](
            const VulkanBatchCompletionSettlement& settlement
        ) {
            return settlement.retirement_serial <=
                   target_retirement_serial;
        }
    );
}

} // namespace Moer::Render
