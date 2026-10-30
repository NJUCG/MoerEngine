#include "VulkanCommandRecorder.h"
#include "VulkanDevice.h"
#include "VulkanMacroUtils.h"
#include "VulkanQueueInternal.h"
#include "misc/Types.h"
#if MOER_VULKAN_TEST_SUPPORT
#include "vulkan/VulkanSubmissionTestHooksInternal.h"
#endif

#include <algorithm>
#include <exception>
#include <limits>

namespace Moer::Render {

void VulkanCommandRecorder::Record(VulkanRecordedCopySubmission& _pending) noexcept {
    assert(GetCurrentRHIThreadRole() == ERHIThreadRole::Translate);
    assert(queue_type == EQueueType::Copy);
    assert(_pending.execution_lease.OwnsGate());
    if (_pending.native_submit_resolved) {
        return;
    }
    try {
        _pending.presentation_source_program = BuildPresentationSourceStateProgram(_pending.submit, queue_type);
        if (device.IsFaulted()) {
            device.RecordRejectedSubmit();
            _pending.retirement_outcome     = {EVulkanOperationStatus::Rejected, device.GetFirstFaultResult()};
            _pending.native_submit_resolved = true;
            return;
        }

        auto reject_before_native_record = [&](const VulkanOperationResult&     _outcome,
                                               std::string_view                 _reason,
                                               UniquePtr<VulkanCommandContext>* _abandoned_command_context = nullptr) {
            try {
                LOG_ERROR(
                    "[RHIExecutor][Vulkan] Copy rejected before native "
                    "record: timeline={} reason={} VkResult={}",
                    _pending.logical_timeline,
                    _reason,
                    static_cast<int32>(_outcome.result)
                );
            } catch (...) {
            }
            if (_abandoned_command_context != nullptr && *_abandoned_command_context) {
                _pending.command_contexts.abandoned.reserve(_pending.command_contexts.abandoned.size() + 1);
                _pending.command_contexts.abandoned.emplace_back(std::move(*_abandoned_command_context));
            }
            device.RecordRejectedSubmit();
            _pending.retirement_outcome    = _outcome;
            _pending.recoverable_rejection = _outcome.status == EVulkanOperationStatus::Rejected &&
                                             !device.IsFaulted() && _outcome.result != VK_ERROR_DEVICE_LOST;
            _pending.native_submit_resolved = true;
        };

        if (!SupportsCompiledCommands(_pending.submit, EQueueType::Copy)) {
            reject_before_native_record(
                {EVulkanOperationStatus::Rejected, VK_ERROR_FEATURE_NOT_PRESENT},
                "Copy submission requires compiled commands and explicit barriers"
            );
            return;
        }

        size_t               timestamp_query_slot_count = 0;
        Array<uint64>        open_timestamp_queries{};
        UnorderedSet<uint64> seen_timestamp_queries{};
        for (const UniquePtr<Command>& command : _pending.submit.cmds) {
            if (!command || command->Type() != Command::EType::Query) {
                continue;
            }
            const auto& query = *static_cast<const QueryCmd*>(command.get());
            if (!query.IsTimestamp() || query.GetQueueType() != EQueueType::Copy || !query.Token().Valid() ||
                query.Token().Kind() != QueryKind::Timestamp) {
                reject_before_native_record(
                    VulkanOperationResult{EVulkanOperationStatus::Rejected, VK_ERROR_FEATURE_NOT_PRESENT},
                    "Copy payload contains a non-Copy timestamp query"
                );
                return;
            }
            const bool token_owned_by_submit = std::any_of(
                _pending.submit.query_tokens.begin(),
                _pending.submit.query_tokens.end(),
                [&query](const QueryToken& _token) {
                    return _token.Valid() && _token.Id() == query.Token().Id() && _token.Kind() == QueryKind::Timestamp;
                }
            );
            if (!token_owned_by_submit) {
                reject_before_native_record(
                    VulkanOperationResult{EVulkanOperationStatus::Rejected, VK_ERROR_FEATURE_NOT_PRESENT},
                    "Copy timestamp query token is not owned by the submit"
                );
                return;
            }
            if (query.IsBegin()) {
                if (!seen_timestamp_queries.emplace(query.Token().Id()).second) {
                    reject_before_native_record(
                        VulkanOperationResult{EVulkanOperationStatus::Rejected, VK_ERROR_FEATURE_NOT_PRESENT},
                        "Copy timestamp query token is recorded more than once"
                    );
                    return;
                }
                open_timestamp_queries.push_back(query.Token().Id());
            } else if (open_timestamp_queries.empty() || open_timestamp_queries.back() != query.Token().Id()) {
                reject_before_native_record(
                    VulkanOperationResult{EVulkanOperationStatus::Rejected, VK_ERROR_FEATURE_NOT_PRESENT},
                    "Copy timestamp query boundaries are malformed"
                );
                return;
            } else {
                open_timestamp_queries.pop_back();
            }
            ++timestamp_query_slot_count;
        }
        if (!open_timestamp_queries.empty()) {
            reject_before_native_record(
                VulkanOperationResult{EVulkanOperationStatus::Rejected, VK_ERROR_FEATURE_NOT_PRESENT},
                "Copy timestamp query boundaries are incomplete"
            );
            return;
        }
        if (timestamp_query_slot_count > std::numeric_limits<uint32>::max()) {
            reject_before_native_record(
                VulkanOperationResult{EVulkanOperationStatus::Rejected, VK_ERROR_OUT_OF_POOL_MEMORY},
                "Copy timestamp query slot count exceeds uint32 capacity"
            );
            return;
        }

        uint32 effective_timestamp_valid_bits = device.GetTimestampValidBits(EQueueType::Copy);
#if MOER_VULKAN_TEST_SUPPORT
        if (timestamp_query_slot_count != 0) {
            const VulkanScriptedTimestampValidBitsOverrideForTesting* scripted_override =
                GetVulkanScriptedTimestampValidBitsOverrideForTesting();
            if (scripted_override != nullptr) {
                effective_timestamp_valid_bits = std::min(
                    effective_timestamp_valid_bits,
                    scripted_override->callback(
                        scripted_override->context, EQueueType::Copy, effective_timestamp_valid_bits
                    )
                );
            }
        }
#endif
        const EVulkanTimestampQueryResetMode timestamp_reset_mode =
            device.GetTimestampQueryResetMode(EQueueType::Copy, effective_timestamp_valid_bits);
        const bool suppress_timestamp_queries =
            timestamp_query_slot_count != 0 && timestamp_reset_mode == EVulkanTimestampQueryResetMode::Unsupported;

        _pending.command_contexts.submitted.reserve(1);
        _pending.command_contexts.abandoned.reserve(1);
        auto command_context_ptr = AcquireContext();
        try {
            if (timestamp_query_slot_count != 0) {
                command_context_ptr->ConfigureTimestampCapabilityForRecording(
                    effective_timestamp_valid_bits, timestamp_reset_mode
                );
            }
            command_context_ptr->EnsureQueryCapacity(suppress_timestamp_queries ? 0 : timestamp_query_slot_count, 0);
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
            return;
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
            return;
        }
        _pending.command_contexts.submitted.emplace_back(std::move(command_context_ptr));
        RecordCopyCommands(*_pending.command_contexts.submitted.front(), _pending.submit, suppress_timestamp_queries);
        return;
    } catch (const VulkanCallError& error) {
        device.RecordRejectedSubmit();
        device.TryLatchFirstFault(_pending.context, error.GetResult(), false);
        _pending.retirement_outcome     = {EVulkanOperationStatus::Faulted, error.GetResult()};
        _pending.native_submit_resolved = true;
    } catch (const std::exception& error) {
        try {
            LOG_ERROR("[RHIExecutor][Vulkan] Copy translation threw: {}", error.what());
        } catch (...) {
        }
    } catch (...) {
        try {
            LOG_ERROR("[RHIExecutor][Vulkan] Copy translation threw");
        } catch (...) {
        }
    }

    if (!_pending.native_submit_resolved) {
        device.RecordRejectedSubmit();
        _pending.retirement_outcome     = {EVulkanOperationStatus::Faulted, VK_ERROR_UNKNOWN};
        _pending.native_submit_resolved = true;
    }
}

} // namespace Moer::Render
