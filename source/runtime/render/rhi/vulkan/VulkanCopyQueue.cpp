#include "VulkanQueueInternal.h"
#include "VulkanDevice.h"
#include "VulkanMacroUtils.h"
#if MOER_VULKAN_TEST_SUPPORT
#include "vulkan/VulkanSubmissionTestHooksInternal.h"
#endif
#include "log/LogSystem.h"
#include "misc/RAII.h"
#include "platform/Platform.h"
#include "rhi/RHIExecutorBackend.h"
#include "rhi/RHIThreadHeartbeat.h"

#include <algorithm>
#include <thread>

namespace Moer::Render {

VkCopyQueue::VkCopyQueue(VulkanDevice& _device) :
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

VkCopyQueue::~VkCopyQueue() {
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
    // Clear recycled command contexts.
    Array<VulkanCommandContext*> recycled_command_contexts;
    command_contexts.PopAll(recycled_command_contexts);
    for (auto& command_context : recycled_command_contexts) {
        MoerDelete(command_context);
    }
    command_context_quarantine.clear();
    timeline = nullptr;
    device.RecordQueueSyncComplete();
}

void VkCopyQueue::EnableRuntimeDependencyWaits() noexcept {
    dependency_waits_enabled.store(true, std::memory_order_release);
}

void VkCopyQueue::CancelRuntimeDependencyWaits() noexcept {
    dependency_waits_enabled.store(false, std::memory_order_release);
}

bool VkCopyQueue::ClaimRuntimeOwnership() {
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

void VkCopyQueue::ReleaseRuntimeOwnership() noexcept {
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

WaitEvent VkCopyQueue::Execute(CmdSubmit&& _evt) {
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

std::optional<VkCopyQueue::CurrentVulkanCopyRecordedSubmit>
VkCopyQueue::TranslateForRuntime(CmdSubmit&& _evt) noexcept {
    assert(
        executor_claimed.load(std::memory_order_acquire) &&
        "runtime Copy translate requires an exclusively claimed queue"
    );
    assert(
        GetCurrentRHIThreadRole() == ERHIThreadRole::Translate &&
        "runtime Copy translation must run on the Translate owner"
    );

    auto execution_lease = runtime_execution_gate.Acquire();
    std::optional<CurrentVulkanCopyRecordedSubmit> recorded{};
    try {
        recorded.emplace(std::move(_evt));
        recorded->execution_lease = std::move(execution_lease);
        recorded->presentation_source_program =
            BuildPresentationSourceStateProgram(
                recorded->submit, EQueueType::Copy
            );

        const bool has_queue_work =
            !recorded->submit.cmds.empty() ||
            !recorded->submit.wait_events.empty() ||
            !recorded->submit.signal_events.empty() ||
            !recorded->submit.callbacks.empty() ||
            !recorded->submit.success_callbacks.empty() ||
            !recorded->submit.gpu_completion_tokens.empty() ||
            !recorded->submit.query_tokens.empty() ||
            recorded->submit.b_sync ||
            recorded->submit.b_tick_profiling ||
            recorded->submit.b_delete_resources;
        if (!has_queue_work) {
            recorded->retirement_outcome     = {};
            recorded->native_submit_resolved = true;
            recorded->completion_committed   = true;
            return recorded;
        }

        std::unique_lock<std::mutex> execution_lock(exec_mutex);
        recorded->logical_timeline =
            last_frame.fetch_add(1, std::memory_order_relaxed) + 1;
        recorded->context = VulkanOperationContext{
            .operation   = EVulkanFaultOperation::QueueSubmit,
            .queue_type  = EQueueType::Copy,
            .queue       = queue.GetHandle(),
            .timeline    = recorded->logical_timeline,
            .work_serial = recorded->logical_timeline,
        };

        if (device.IsFaulted()) {
            device.RecordRejectedSubmit();
            recorded->retirement_outcome = {
                EVulkanOperationStatus::Rejected,
                device.GetFirstFaultResult()
            };
            recorded->native_submit_resolved = true;
            return recorded;
        }

        auto reject_before_native_record =
            [&](const VulkanOperationResult&     _outcome,
                std::string_view                 _reason,
                UniquePtr<VulkanCommandContext>* _abandoned_command_context = nullptr) {
                try {
                    LOG_ERROR(
                        "[RHIExecutor][Vulkan] Copy rejected before native "
                        "record: timeline={} reason={} VkResult={}",
                        recorded->logical_timeline,
                        _reason,
                        static_cast<int32_t>(_outcome.result)
                    );
                } catch (...) {
                }
                if (_abandoned_command_context != nullptr && *_abandoned_command_context) {
                    recorded->command_contexts.abandoned.reserve(
                        recorded->command_contexts.abandoned.size() + 1
                    );
                    recorded->command_contexts.abandoned.emplace_back(std::move(*_abandoned_command_context));
                }
                device.RecordRejectedSubmit();
                recorded->retirement_outcome = _outcome;
                recorded->recoverable_rejection =
                    _outcome.status ==
                        EVulkanOperationStatus::Rejected &&
                    !device.IsFaulted() &&
                    _outcome.result != VK_ERROR_DEVICE_LOST;
                recorded->native_submit_resolved = true;
            };

        if (!SupportsVulkanCompiledCommands(recorded->submit, EQueueType::Copy)) {
            reject_before_native_record(
                {EVulkanOperationStatus::Rejected, VK_ERROR_FEATURE_NOT_PRESENT},
                "Copy submission requires compiled commands and explicit barriers"
            );
            return recorded;
        }

        size_t               timestamp_query_slot_count = 0;
        Array<uint64>         open_timestamp_queries{};
        UnorderedSet<uint64>  seen_timestamp_queries{};
        for (const UniquePtr<Command>& command :
             recorded->submit.cmds) {
            if (!command ||
                command->Type() != Command::EType::Query) {
                continue;
            }
            const auto& query =
                *static_cast<const QueryCmd*>(command.get());
            if (!query.IsTimestamp() ||
                query.GetQueueType() != EQueueType::Copy ||
                !query.Token().Valid() ||
                query.Token().Kind() != QueryKind::Timestamp) {
                reject_before_native_record(
                    VulkanOperationResult{
                        EVulkanOperationStatus::Rejected,
                        VK_ERROR_FEATURE_NOT_PRESENT
                    },
                    "Copy payload contains a non-Copy timestamp query"
                );
                return recorded;
            }
            const bool token_owned_by_submit = std::any_of(
                recorded->submit.query_tokens.begin(),
                recorded->submit.query_tokens.end(),
                [&query](const QueryToken& _token) {
                    return _token.Valid() &&
                           _token.Id() == query.Token().Id() &&
                           _token.Kind() == QueryKind::Timestamp;
                }
            );
            if (!token_owned_by_submit) {
                reject_before_native_record(
                    VulkanOperationResult{
                        EVulkanOperationStatus::Rejected,
                        VK_ERROR_FEATURE_NOT_PRESENT
                    },
                    "Copy timestamp query token is not owned by the submit"
                );
                return recorded;
            }
            if (query.IsBegin()) {
                if (!seen_timestamp_queries.emplace(
                        query.Token().Id()
                    ).second) {
                    reject_before_native_record(
                        VulkanOperationResult{
                            EVulkanOperationStatus::Rejected,
                            VK_ERROR_FEATURE_NOT_PRESENT
                        },
                        "Copy timestamp query token is recorded more than once"
                    );
                    return recorded;
                }
                open_timestamp_queries.push_back(query.Token().Id());
            } else if (open_timestamp_queries.empty() ||
                       open_timestamp_queries.back() !=
                           query.Token().Id()) {
                reject_before_native_record(
                    VulkanOperationResult{
                        EVulkanOperationStatus::Rejected,
                        VK_ERROR_FEATURE_NOT_PRESENT
                    },
                    "Copy timestamp query boundaries are malformed"
                );
                return recorded;
            } else {
                open_timestamp_queries.pop_back();
            }
            ++timestamp_query_slot_count;
        }
        if (!open_timestamp_queries.empty()) {
            reject_before_native_record(
                VulkanOperationResult{
                    EVulkanOperationStatus::Rejected,
                    VK_ERROR_FEATURE_NOT_PRESENT
                },
                "Copy timestamp query boundaries are incomplete"
            );
            return recorded;
        }
        if (timestamp_query_slot_count >
            std::numeric_limits<uint32>::max()) {
            reject_before_native_record(
                VulkanOperationResult{
                    EVulkanOperationStatus::Rejected,
                    VK_ERROR_OUT_OF_POOL_MEMORY
                },
                "Copy timestamp query slot count exceeds uint32 capacity"
            );
            return recorded;
        }

        uint32 effective_timestamp_valid_bits =
            device.GetTimestampValidBits(EQueueType::Copy);
#if MOER_VULKAN_TEST_SUPPORT
        if (timestamp_query_slot_count != 0) {
            const VulkanScriptedTimestampValidBitsOverrideForTesting*
                scripted_override =
                    GetVulkanScriptedTimestampValidBitsOverrideForTesting();
            if (scripted_override != nullptr) {
                effective_timestamp_valid_bits = std::min(
                    effective_timestamp_valid_bits,
                    scripted_override->callback(
                        scripted_override->context,
                        EQueueType::Copy,
                        effective_timestamp_valid_bits
                    )
                );
            }
        }
#endif
        const EVulkanTimestampQueryResetMode timestamp_reset_mode =
            device.GetTimestampQueryResetMode(
                EQueueType::Copy,
                effective_timestamp_valid_bits
            );
        const bool suppress_timestamp_queries =
            timestamp_query_slot_count != 0 &&
            timestamp_reset_mode ==
                EVulkanTimestampQueryResetMode::Unsupported;

        recorded->command_contexts.submitted.reserve(1);
        recorded->command_contexts.abandoned.reserve(1);
        auto command_context_ptr = AcquireCommandContext();
        try {
            if (timestamp_query_slot_count != 0) {
                command_context_ptr->ConfigureTimestampCapabilityForRecording(
                    effective_timestamp_valid_bits, timestamp_reset_mode
                );
            }
            command_context_ptr->EnsureQueryCapacity(
                suppress_timestamp_queries ? 0 : timestamp_query_slot_count, 0
            );
        } catch (const std::exception& error) {
            try {
                LOG_ERROR(
                    "[RHIExecutor][Vulkan] Copy query pool preparation "
                    "failed: {}",
                    error.what()
                );
            } catch (...) {
            }
            const bool device_faulted = device.IsFaulted();
            reject_before_native_record(
                VulkanOperationResult{
                    device_faulted ? EVulkanOperationStatus::Faulted : EVulkanOperationStatus::Rejected,
                    device_faulted ? device.GetFirstFaultResult() : VK_ERROR_OUT_OF_POOL_MEMORY
                },
                "Copy query pool preparation failed",
                &command_context_ptr
            );
            return recorded;
        } catch (...) {
            const bool device_faulted = device.IsFaulted();
            reject_before_native_record(
                VulkanOperationResult{
                    device_faulted ? EVulkanOperationStatus::Faulted : EVulkanOperationStatus::Rejected,
                    device_faulted ? device.GetFirstFaultResult() : VK_ERROR_OUT_OF_POOL_MEMORY
                },
                "Copy query pool preparation failed",
                &command_context_ptr
            );
            return recorded;
        }
        recorded->command_contexts.submitted.emplace_back(std::move(command_context_ptr));
        RecordVulkanCopyCommands(
            device, *recorded->command_contexts.submitted.front(), recorded->submit, suppress_timestamp_queries
        );
        return recorded;
    } catch (const VulkanCallError& error) {
        device.RecordRejectedSubmit();
        device.TryLatchFirstFault(recorded->context, error.GetResult(), false);
        recorded->retirement_outcome = {EVulkanOperationStatus::Faulted, error.GetResult()};
        recorded->native_submit_resolved = true;
    } catch (const std::exception& error) {
        try {
            LOG_ERROR(
                "[RHIExecutor][Vulkan] Copy translation threw: {}",
                error.what()
            );
        } catch (...) {
        }
    } catch (...) {
        try {
            LOG_ERROR("[RHIExecutor][Vulkan] Copy translation threw");
        } catch (...) {
        }
    }

    if (!recorded) {
        recorded.emplace(std::move(_evt));
        recorded->execution_lease = std::move(execution_lease);
    }
    if (!recorded->native_submit_resolved) {
        device.RecordRejectedSubmit();
        recorded->retirement_outcome = {
            EVulkanOperationStatus::Faulted, VK_ERROR_UNKNOWN
        };
        recorded->native_submit_resolved = true;
    }
    return recorded;
}

VulkanRuntimeSubmissionResult VkCopyQueue::SubmitRecordedForRuntime(
    CurrentVulkanCopyRecordedSubmit       _recorded,
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
    VulkanRuntimeSubmissionResult runtime_result{
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

void VkCopyQueue::RejectRecordedForRuntime(
    CurrentVulkanCopyRecordedSubmit&& _recorded,
    VkResult                           _result,
    bool                               _recoverable
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
void VkCopyQueue::RejectForRuntime(
    CmdSubmit&& _submit,
    VkResult    _result,
    bool        _recoverable,
    VulkanBatchCompletionTicket _batch_completion
) noexcept {
    device.RecordRejectedSubmit();
    CurrentVulkanCopyRecordedSubmit rejected{std::move(_submit)};
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

void VkCopyQueue::CommitRuntimeSubmitCompletion(
    CurrentVulkanCopyRecordedSubmit& _recorded,
    const VulkanOperationResult&      _outcome
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

void VkCopyQueue::Sync(uint64 _timeline) {
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

FenceRef VkCopyQueue::GetFenceHandle() {
    return FenceRef(timeline);
}

void VkCopyQueue::ExecuteThread() {
    Platform::SetCurrentThreadName("Vulkan Copy Completion");
    RHIThreadRoleScope owner_scope(ERHIThreadRole::Completion);
    RHIThreadHeartbeatScope heartbeat(
        ERHIThreadRole::Completion,
        ERHIHeartbeatDomain::Copy,
        ERHIHeartbeatStage::PollCompletion
    );
    while (true) {
        std::optional<CompletionEvent> evt;
        {
            heartbeat.Pulse(ERHIHeartbeatStage::PollCompletion);
            std::unique_lock<std::mutex> lock(event_mutex);
            RHIThreadHeartbeatWaitLock wait_lock(
                lock, heartbeat, ERHIHeartbeatStage::PollCompletion
            );
            queue_cv.wait(wait_lock, [this]() {
                return !enabled.load(std::memory_order_acquire) || !event_queue.empty();
            });
            if (!enabled.load(std::memory_order_acquire) && event_queue.empty()) {
                heartbeat.Pulse(ERHIHeartbeatStage::Shutdown);
                break;
            }
            evt.emplace(std::move(event_queue.front()));
            event_queue.pop_front();
        }

        heartbeat.Pulse(ERHIHeartbeatStage::AwaitCompletion);
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

        if (recycle_batch) {
            for (auto& command_context : batch.command_contexts.submitted) {
                recycle_batch = command_context->Reset() && recycle_batch;
            }
        }
        if (recycle_batch && !device.IsFaulted()) {
            for (auto& command_context : batch.command_contexts.submitted) {
                command_contexts.Push(command_context.release());
            }
        } else {
            for (auto& command_context : batch.command_contexts.submitted) {
                if (!command_context) {
                    continue;
                }
                device.RecordAllocatorQuarantine();
                device.RecordSkippedCommandPoolReset();
                command_context_quarantine.emplace_back(std::move(command_context));
            }
        }
        for (auto& command_context : batch.command_contexts.abandoned) {
            if (!device.IsFaulted() && command_context->ResetAbandoned()) {
                command_contexts.Push(command_context.release());
            } else if (command_context) {
                device.RecordAllocatorQuarantine();
                device.RecordSkippedCommandPoolReset();
                command_context_quarantine.emplace_back(std::move(command_context));
            }
        }

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

UniquePtr<VulkanCommandContext> VkCopyQueue::AcquireCommandContext() {
    auto command_context = std::move(UniquePtr<VulkanCommandContext>(command_contexts.Pop()));
    if (command_context) {
        return std::move(command_context);
    }
    return MakeUnique<VulkanCommandContext>(&device, EQueueType::Copy);
}

void VkCopyQueue::CompleteAll(uint64 _timeline) {
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
