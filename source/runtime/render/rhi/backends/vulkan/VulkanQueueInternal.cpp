#include "VulkanQueueInternal.h"
#include "VulkanDevice.h"
#include "log/LogSystem.h"
#if MOER_VULKAN_TEST_SUPPORT
#include "vulkan/VulkanSubmissionTestHooksInternal.h"
#endif

#include <exception>

namespace Moer::Render {

void FinalizeBindlessUpdatesOnce(
    BindlessArrayRef                         _array,
    const Array<BindlessArray::UpdateCmd>&   _updates,
    const std::shared_ptr<std::atomic_bool>& _finalized,
    bool                                     _gpu_update_succeeded
) {
    if (!_array || !_finalized || _finalized->exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    try {
        static_cast<VulkanBindlessArray*>(_array.Get())->FinalizeUpdateCommand(_updates, _gpu_update_succeeded);
    } catch (const std::exception& error) {
        LogQueueErrorNoexcept("Failed to finalize bindless update lifecycle: {}", error.what());
    } catch (...) {
        LogQueueErrorNoexcept("Failed to finalize bindless update lifecycle: unknown error");
    }
}

void FinalizeRejectedBindlessUpdates(const CmdSubmit& _submit) {
    for (const UniquePtr<Command>& command : _submit.cmds) {
        if (!command || command->Type() != Command::EType::UpdateBindlessArray) {
            continue;
        }
        const auto& update = *static_cast<const UpdateBindlessArrayCmd*>(command.get());
        if (!update.UpdatesHandedOff()) {
            continue;
        }
        FinalizeBindlessUpdatesOnce(
            BindlessArrayRef(update.Handle()), update.UpdateCommands(), update.UpdateFinalizationToken(), false
        );
    }
}

void InvokeCallbacksNoexcept(Array<std::function<void()>>& _callbacks, std::string_view _context) noexcept {
    for (std::function<void()>& callback : _callbacks) {
        if (!callback) {
            continue;
        }
        try {
            callback();
        } catch (const std::exception& error) {
            LogQueueErrorNoexcept("{} callback threw: {}", _context, error.what());
        } catch (...) {
            LogQueueErrorNoexcept("{} callback threw an unknown exception", _context);
        }
    }
}

bool WaitForSubmittedDependencies(
    VulkanDevice&           _device,
    const Array<WaitEvent>& _wait_events,
    EQueueType              _queue,
    const std::atomic_bool* _continue_waiting
) {
    if (!_wait_events.empty()) {
#if MOER_VULKAN_TEST_SUPPORT
        NotifyVulkanSubmissionDependencyWaitBegin(_queue, static_cast<uint32>(_wait_events.size()));
#endif
    }
    for (const WaitEvent& event : _wait_events) {
        auto* fence = reinterpret_cast<VulkanTimeline*>(event.timeline_handle);
        if (fence == nullptr || !fence->WaitSubmitted(event.value, _continue_waiting)) {
            return false;
        }
    }
    return !_device.IsFaulted();
}

VkResult GetRejectedSubmitResult(VulkanDevice& _device) {
    return _device.IsFaulted() ? _device.GetFirstFaultResult() : VK_ERROR_UNKNOWN;
}

VulkanOperationResult MakeUnsubmittedOutcome(VkResult _result, bool _recoverable) {
    if (_result == VK_SUCCESS) {
        _result = VK_ERROR_UNKNOWN;
    }
    return {_recoverable ? EVulkanOperationStatus::Rejected : EVulkanOperationStatus::Faulted, _result};
}

void MarkSubmissionAccepted(
    VulkanTimeline*           _queue_timeline,
    uint64                    _timeline,
    const Array<SignalEvent>& _signal_events
) {
    _queue_timeline->MarkSubmitted(_timeline);
    for (const SignalEvent& event : _signal_events) {
        reinterpret_cast<VulkanTimeline*>(event.timeline_handle)->MarkSubmitted(event.value);
    }
}

static bool IsRecoverableUnsubmittedSignal(VulkanDevice& _device, const VulkanOperationResult& _outcome) {
    // A skipped Present reserves a logical value without submitting a GPU
    // signal. Cancel only that value; later submissions still use this fence.
    return (_outcome.status == EVulkanOperationStatus::Rejected || _outcome.status == EVulkanOperationStatus::Retry ||
            _outcome.status == EVulkanOperationStatus::Recreate) &&
           !_device.IsFaulted() && _outcome.result != VK_ERROR_DEVICE_LOST;
}

void TerminalizeUnsubmittedSignal(
    VulkanDevice&                _device,
    VulkanTimeline*              _fence,
    uint64                       _value,
    const VulkanOperationResult& _outcome
) {
    if (_fence == nullptr) {
        return;
    }
    if (IsRecoverableUnsubmittedSignal(_device, _outcome)) {
        _fence->Reject(_value);
    } else {
        const VkResult result = _outcome.result != VK_SUCCESS ? _outcome.result : VK_ERROR_UNKNOWN;
        _fence->Fail(result);
    }
}

void TerminalizeUnsubmittedSignals(
    VulkanDevice&                _device,
    const Array<SignalEvent>&    _signal_events,
    const VulkanOperationResult& _outcome
) {
    for (const SignalEvent& event : _signal_events) {
        TerminalizeUnsubmittedSignal(
            _device, reinterpret_cast<VulkanTimeline*>(event.timeline_handle), event.value, _outcome
        );
    }
}

QueryPublishBatch PublishUnsubmittedQueryErrors(
    CmdSubmit&                 _submit,
    VulkanCommandContextBatch* _command_contexts,
    std::string_view           _reason
) noexcept {
    const QueryPublishBatch query_batch =
        _submit.query_publish_batch.Valid() ? _submit.query_publish_batch : QueryBackendAccess::BeginPublishBatch();

    if (_command_contexts != nullptr) {
        for (auto& command_context : _command_contexts->submitted) {
            if (command_context) {
                command_context->PublishQueriesAfterGpuCompletion(query_batch, false, _reason);
            }
        }
        for (auto& command_context : _command_contexts->abandoned) {
            if (command_context) {
                command_context->PublishQueriesAfterGpuCompletion(query_batch, false, _reason);
            }
        }
    }
    // The submit-token pass is also the malformed/native-record fallback in
    // the opposite direction: a token without a command context record still
    // becomes terminal in the same transaction.
    _submit.PublishPendingQueryErrors(_reason, query_batch);
    return query_batch;
}

} // namespace Moer::Render
