#pragma once

#include "VulkanSubmissionDiagnostics.h"

#include <cstddef>

namespace Moer::Render {

struct VulkanRuntimeSubmissionResult;

// Called by the runtime owners at the existing operation boundaries. Notifications
// are synchronous; they do not schedule work or extend observer lifetimes.
struct VulkanSubmissionBoundaryIdentity {
    uint64                       batch_sequence{0};
    uint32                       operation_index{0};
    EQueueType                   queue{EQueueType::Ignore};
    EVulkanSubmissionBoundaryKind kind{
        EVulkanSubmissionBoundaryKind::SerialControl
    };
    uint32                       dependency_wait_count{0};
};

void NotifyVulkanNativeSubmission(
    EQueueType                   _queue,
    VkQueue                      _native_queue,
    const VulkanOperationResult& _outcome,
    bool                         _empty_submit
) noexcept;

void NotifyVulkanQueueLocalSyncWait(
    EQueueType _queue,
    uint64     _target_retirement_serial,
    size_t     _completion_group_count
) noexcept;

void NotifyVulkanDeviceIdleWait(const VulkanOperationContext& _context) noexcept;

void NotifyVulkanSubmissionDependencyWaitBegin(
    EQueueType _queue,
    uint32     _dependency_count
) noexcept;

void NotifyVulkanBackendSyncWait() noexcept;

void NotifyVulkanSubmissionBoundary(
    const VulkanSubmissionBoundaryIdentity&    _identity,
    EVulkanSubmissionBoundaryPhase       _phase,
    const VulkanRuntimeSubmissionResult* _result = nullptr,
    uint32 _present_receipt_resolution_attempts = 0
) noexcept;

void NotifyVulkanSourceSubmitted(
    uint64     _batch_sequence,
    uint32     _source_index,
    uint32     _original_source_index,
    uint32     _source_segment_index,
    uint32     _source_segment_count,
    EQueueType _queue,
    uint64     _async_queue_scope,
    bool       _cross_native_predecessor_wait
) noexcept;

void NotifyVulkanSourceTranslation(
    uint64                        _batch_sequence,
    uint32                        _source_index,
    uint32                        _original_source_index,
    uint32                        _source_segment_index,
    uint32                        _source_segment_count,
    EQueueType                    _queue,
    uint32                        _native_queue_id,
    uint64                        _async_queue_scope,
    EVulkanSourceTranslationPhase _phase,
    bool                          _parallel_record_requested = false,
    bool                          _parallel_record_planned = false,
    bool                          _parallel_record_effective = false
) noexcept;

void NotifyVulkanBatchPreflightRejected(
    uint64 _batch_sequence,
    bool   _executable_preflight
) noexcept;

// Preserve a single acquired override pointer across each operation's checks.
// The installer must quiesce that operation before removing/destroying the override.
const VulkanScriptedQueryPreparationOverrideForTesting* GetVulkanScriptedQueryPreparationOverrideForTesting() noexcept;
const VulkanScriptedTimestampValidBitsOverrideForTesting* GetVulkanScriptedTimestampValidBitsOverrideForTesting() noexcept;
const VulkanScriptedPresentOverrideForTesting* GetVulkanScriptedPresentOverrideForTesting() noexcept;

} // namespace Moer::Render
