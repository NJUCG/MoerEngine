#include "VulkanSubmissionTestHooksInternal.h"

#include "platform/Platform.h"
#include "rhi/RHI.h"
#include "rhi/vulkan/VulkanDevice.h"
#include "rhi/vulkan/VulkanQueue.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <limits>

namespace Moer::Render {
namespace {

std::atomic<const VulkanNativeSubmissionObserver*>
    g_native_submission_observer{nullptr};
std::atomic<const VulkanSubmissionDependencyWaitObserver*>
    g_submission_dependency_wait_observer{nullptr};
std::atomic<const VulkanBackendSyncWaitObserver*>
    g_backend_sync_wait_observer{nullptr};
std::atomic<const VulkanQueueLocalSyncWaitObserver*>
    g_queue_local_sync_wait_observer{nullptr};
std::atomic<const VulkanDeviceIdleWaitObserver*> g_device_idle_wait_observer{nullptr};
std::atomic<const VulkanScriptedQueryPreparationOverrideForTesting*>
    g_scripted_query_preparation_override{nullptr};
std::atomic<const VulkanScriptedTimestampValidBitsOverrideForTesting*>
    g_scripted_timestamp_valid_bits_override{nullptr};
std::atomic<const VulkanScriptedPresentOverrideForTesting*>
    g_scripted_present_override{nullptr};
std::atomic<const VulkanSourceSubmissionObserver*> g_source_submission_observer{nullptr};
std::atomic<const VulkanSourceTranslationObserver*>
    g_source_translation_observer{nullptr};
std::atomic<const VulkanBatchPreflightRejectionObserver*>
    g_batch_preflight_rejection_observer{nullptr};
std::atomic<const VulkanSubmissionBoundaryObserver*>
    g_submission_boundary_observer{nullptr};

template<typename Observer>
bool TryInstallObserver(std::atomic<const Observer*>& _slot, const Observer* _observer) noexcept {
    if (_observer == nullptr || _observer->callback == nullptr) {
        return false;
    }
    const Observer* expected = nullptr;
    return _slot.compare_exchange_strong(
        expected, _observer, std::memory_order_release, std::memory_order_relaxed
    );
}

template<typename Observer>
bool RemoveObserver(std::atomic<const Observer*>& _slot, const Observer* _observer) noexcept {
    if (_observer == nullptr) {
        return false;
    }
    const Observer* expected = _observer;
    // Detaching the slot does not wait for callbacks that already acquired it.
    return _slot.compare_exchange_strong(
        expected, nullptr, std::memory_order_acq_rel, std::memory_order_acquire
    );
}

} // namespace

bool TryInstallVulkanNativeSubmissionObserver(
    const VulkanNativeSubmissionObserver* _observer
) noexcept {
    return TryInstallObserver(g_native_submission_observer, _observer);
}

bool RemoveVulkanNativeSubmissionObserver(
    const VulkanNativeSubmissionObserver* _observer
) noexcept {
    return RemoveObserver(g_native_submission_observer, _observer);
}

bool TryInstallVulkanScriptedQueryPreparationOverrideForTesting(
    const VulkanScriptedQueryPreparationOverrideForTesting* _override
) noexcept {
    return TryInstallObserver(g_scripted_query_preparation_override, _override);
}

bool RemoveVulkanScriptedQueryPreparationOverrideForTesting(
    const VulkanScriptedQueryPreparationOverrideForTesting* _override
) noexcept {
    return RemoveObserver(g_scripted_query_preparation_override, _override);
}

bool TryInstallVulkanScriptedTimestampValidBitsOverrideForTesting(
    const VulkanScriptedTimestampValidBitsOverrideForTesting* _override
) noexcept {
    return TryInstallObserver(g_scripted_timestamp_valid_bits_override, _override);
}

bool RemoveVulkanScriptedTimestampValidBitsOverrideForTesting(
    const VulkanScriptedTimestampValidBitsOverrideForTesting* _override
) noexcept {
    return RemoveObserver(g_scripted_timestamp_valid_bits_override, _override);
}

bool TryInstallVulkanScriptedPresentOverrideForTesting(
    const VulkanScriptedPresentOverrideForTesting* _override
) noexcept {
    return TryInstallObserver(g_scripted_present_override, _override);
}

bool RemoveVulkanScriptedPresentOverrideForTesting(
    const VulkanScriptedPresentOverrideForTesting* _override
) noexcept {
    return RemoveObserver(g_scripted_present_override, _override);
}

bool TryInstallVulkanSubmissionDependencyWaitObserver(
    const VulkanSubmissionDependencyWaitObserver* _observer
) noexcept {
    return TryInstallObserver(g_submission_dependency_wait_observer, _observer);
}

bool RemoveVulkanSubmissionDependencyWaitObserver(
    const VulkanSubmissionDependencyWaitObserver* _observer
) noexcept {
    return RemoveObserver(g_submission_dependency_wait_observer, _observer);
}

bool TryInstallVulkanBackendSyncWaitObserver(
    const VulkanBackendSyncWaitObserver* _observer
) noexcept {
    return TryInstallObserver(g_backend_sync_wait_observer, _observer);
}

bool RemoveVulkanBackendSyncWaitObserver(
    const VulkanBackendSyncWaitObserver* _observer
) noexcept {
    return RemoveObserver(g_backend_sync_wait_observer, _observer);
}

bool TryInstallVulkanQueueLocalSyncWaitObserver(
    const VulkanQueueLocalSyncWaitObserver* _observer
) noexcept {
    return TryInstallObserver(g_queue_local_sync_wait_observer, _observer);
}

bool RemoveVulkanQueueLocalSyncWaitObserver(
    const VulkanQueueLocalSyncWaitObserver* _observer
) noexcept {
    return RemoveObserver(g_queue_local_sync_wait_observer, _observer);
}

bool TryInstallVulkanDeviceIdleWaitObserver(const VulkanDeviceIdleWaitObserver* _observer) noexcept {
    return TryInstallObserver(g_device_idle_wait_observer, _observer);
}

bool RemoveVulkanDeviceIdleWaitObserver(const VulkanDeviceIdleWaitObserver* _observer) noexcept {
    return RemoveObserver(g_device_idle_wait_observer, _observer);
}

bool TryInstallVulkanSourceSubmissionObserver(const VulkanSourceSubmissionObserver* _observer) noexcept {
    return TryInstallObserver(g_source_submission_observer, _observer);
}

bool RemoveVulkanSourceSubmissionObserver(const VulkanSourceSubmissionObserver* _observer) noexcept {
    return RemoveObserver(g_source_submission_observer, _observer);
}

bool TryInstallVulkanSubmissionBoundaryObserver(
    const VulkanSubmissionBoundaryObserver* _observer
) noexcept {
    return TryInstallObserver(g_submission_boundary_observer, _observer);
}

bool RemoveVulkanSubmissionBoundaryObserver(
    const VulkanSubmissionBoundaryObserver* _observer
) noexcept {
    return RemoveObserver(g_submission_boundary_observer, _observer);
}

bool TryInstallVulkanSourceTranslationObserver(
    const VulkanSourceTranslationObserver* _observer
) noexcept {
    return TryInstallObserver(g_source_translation_observer, _observer);
}

bool RemoveVulkanSourceTranslationObserver(
    const VulkanSourceTranslationObserver* _observer
) noexcept {
    return RemoveObserver(g_source_translation_observer, _observer);
}

bool TryInstallVulkanBatchPreflightRejectionObserver(
    const VulkanBatchPreflightRejectionObserver* _observer
) noexcept {
    return TryInstallObserver(g_batch_preflight_rejection_observer, _observer);
}

bool RemoveVulkanBatchPreflightRejectionObserver(
    const VulkanBatchPreflightRejectionObserver* _observer
) noexcept {
    return RemoveObserver(g_batch_preflight_rejection_observer, _observer);
}

void NotifyVulkanNativeSubmission(
    EQueueType                   _queue,
    VkQueue                      _native_queue,
    const VulkanOperationResult& _outcome,
    bool                         _empty_submit
) noexcept {
    const VulkanNativeSubmissionObserver* observer =
        g_native_submission_observer.load(std::memory_order_acquire);
    if (observer == nullptr) {
        return;
    }
    observer->callback(
        observer->context,
        VulkanNativeSubmissionEvent{
            .queue               = _queue,
            .native_queue_handle = reinterpret_cast<uint64>(_native_queue),
            .thread_id           = Platform::GetCurrentThreadID(),
            .thread_role         = GetCurrentRHIThreadRole(),
            .outcome             = _outcome,
            .empty_submit        = _empty_submit,
        }
    );
}

void NotifyVulkanQueueLocalSyncWait(
    EQueueType _queue,
    uint64     _target_retirement_serial,
    size_t     _completion_group_count
) noexcept {
    const VulkanQueueLocalSyncWaitObserver* observer =
        g_queue_local_sync_wait_observer.load(
            std::memory_order_acquire
        );
    if (observer == nullptr) {
        return;
    }
    observer->callback(
        observer->context,
        VulkanQueueLocalSyncWaitEvent{
            .queue       = _queue,
            .thread_id   = Platform::GetCurrentThreadID(),
            .thread_role = GetCurrentRHIThreadRole(),
            .target_retirement_serial =
                _target_retirement_serial,
            .completion_group_count = static_cast<uint32>(
                std::min(
                    _completion_group_count,
                    static_cast<size_t>(
                        std::numeric_limits<uint32>::max()
                    )
                )
            ),
        }
    );
}

void NotifyVulkanDeviceIdleWait(const VulkanOperationContext& _context) noexcept {
    const VulkanDeviceIdleWaitObserver* observer =
        g_device_idle_wait_observer.load(std::memory_order_acquire);
    if (observer == nullptr) {
        return;
    }
    observer->callback(
        observer->context,
        VulkanDeviceIdleWaitEvent{
            .context     = _context,
            .thread_id   = Platform::GetCurrentThreadID(),
            .thread_role = GetCurrentRHIThreadRole(),
        }
    );
}

void NotifyVulkanSubmissionDependencyWaitBegin(
    EQueueType _queue,
    uint32     _dependency_count
) noexcept {
    const VulkanSubmissionDependencyWaitObserver* observer =
        g_submission_dependency_wait_observer.load(
            std::memory_order_acquire
        );
    if (observer == nullptr) {
        return;
    }
    observer->callback(
        observer->context,
        VulkanSubmissionDependencyWaitEvent{
            .queue            = _queue,
            .thread_id        = Platform::GetCurrentThreadID(),
            .thread_role      = GetCurrentRHIThreadRole(),
            .dependency_count = _dependency_count,
        }
    );
}

void NotifyVulkanBackendSyncWait() noexcept {
    const VulkanBackendSyncWaitObserver* observer =
        g_backend_sync_wait_observer.load(std::memory_order_acquire);
    if (observer == nullptr) {
        return;
    }
    observer->callback(
        observer->context,
        VulkanBackendSyncWaitEvent{
            .thread_id   = Platform::GetCurrentThreadID(),
            .thread_role = GetCurrentRHIThreadRole(),
        }
    );
}

void NotifyVulkanSubmissionBoundary(
    const VulkanSubmissionBoundaryIdentity& _identity,
    EVulkanSubmissionBoundaryPhase          _phase,
    const VulkanSubmissionResult*           _result,
    uint32                                  _present_receipt_resolution_attempts
) noexcept {
    assert(
        GetCurrentRHIThreadRole() == ERHIThreadRole::Submission &&
        "submission boundary diagnostics must run on the Submission owner"
    );
    const VulkanSubmissionBoundaryObserver* observer =
        g_submission_boundary_observer.load(std::memory_order_acquire);
    if (observer == nullptr) {
        return;
    }
    observer->callback(
        observer->context,
        VulkanSubmissionBoundaryEvent{
            .batch_sequence       = _identity.batch_sequence,
            .operation_index      = _identity.operation_index,
            .queue                 = _identity.queue,
            .kind                  = _identity.kind,
            .phase                 = _phase,
            .dependency_wait_count =
                _identity.dependency_wait_count,
            .thread_id             = Platform::GetCurrentThreadID(),
            .thread_role           = GetCurrentRHIThreadRole(),
            .outcome               =
                _result != nullptr ?
                    _result->outcome :
                    VulkanOperationResult{},
            .outcome_valid         = _result != nullptr,
            .gpu_submitted =
                _result != nullptr && _result->WasSubmitted(),
            .recoverable_rejection =
                _result != nullptr &&
                _result->IsRecoverableRejection(),
            .present_receipt_resolution_attempts =
                _present_receipt_resolution_attempts,
        }
    );
}

void NotifyVulkanSourceSubmitted(
    uint64     _batch_sequence,
    uint32     _source_index,
    uint32     _original_source_index,
    uint32     _source_segment_index,
    uint32     _source_segment_count,
    EQueueType _queue,
    uint64     _async_queue_scope,
    bool       _cross_native_predecessor_wait
) noexcept {
    assert(
        GetCurrentRHIThreadRole() == ERHIThreadRole::Submission &&
        "source submission diagnostics must run on the Submission owner"
    );
    const VulkanSourceSubmissionObserver* observer =
        g_source_submission_observer.load(std::memory_order_acquire);
    if (observer == nullptr) {
        return;
    }
    observer->callback(
        observer->context,
        VulkanSourceSubmissionEvent{
            .batch_sequence    = _batch_sequence,
            .source_index      = _source_index,
            .original_source_index         = _original_source_index,
            .source_segment_index          = _source_segment_index,
            .source_segment_count          = _source_segment_count,
            .queue             = _queue,
            .async_queue_scope = _async_queue_scope,
            .cross_native_predecessor_wait = _cross_native_predecessor_wait,
        }
    );
}

void NotifyVulkanSourceTranslation(
    uint64                        _batch_sequence,
    uint32                        _source_index,
    uint32                        _original_source_index,
    uint32                        _source_segment_index,
    uint32                        _source_segment_count,
    EQueueType                    _queue,
    uint32                        _native_queue_id,
    uint64                        _async_queue_scope,
    EVulkanSourceTranslationPhase _phase
) noexcept {
    assert(
        GetCurrentRHIThreadRole() == ERHIThreadRole::Translate &&
        "source translation diagnostics must run on a Translate owner"
    );
    const VulkanSourceTranslationObserver* observer =
        g_source_translation_observer.load(
            std::memory_order_acquire
        );
    if (observer == nullptr) {
        return;
    }
    observer->callback(
        observer->context,
        VulkanSourceTranslationEvent{
            .batch_sequence       = _batch_sequence,
            .source_index         = _source_index,
            .original_source_index = _original_source_index,
            .source_segment_index = _source_segment_index,
            .source_segment_count = _source_segment_count,
            .queue                 = _queue,
            .native_queue_id       = _native_queue_id,
            .async_queue_scope     = _async_queue_scope,
            .thread_id             = Platform::GetCurrentThreadID(),
            .thread_role           = GetCurrentRHIThreadRole(),
            .phase                 = _phase,
        }
    );
}

void NotifyVulkanBatchPreflightRejected(
    uint64 _batch_sequence,
    bool   _executable_preflight
) noexcept {
    assert(
        GetCurrentRHIThreadRole() == ERHIThreadRole::Translate &&
        "batch preflight diagnostics must run on the Translate owner"
    );
    const VulkanBatchPreflightRejectionObserver* observer =
        g_batch_preflight_rejection_observer.load(
            std::memory_order_acquire
        );
    if (observer == nullptr) {
        return;
    }
    observer->callback(
        observer->context,
        VulkanBatchPreflightRejectionEvent{
            .batch_sequence       = _batch_sequence,
            .thread_id            = Platform::GetCurrentThreadID(),
            .thread_role          = GetCurrentRHIThreadRole(),
            .executable_preflight = _executable_preflight,
        }
    );
}

const VulkanScriptedQueryPreparationOverrideForTesting* GetVulkanScriptedQueryPreparationOverrideForTesting() noexcept {
    return g_scripted_query_preparation_override.load(std::memory_order_acquire);
}

const VulkanScriptedTimestampValidBitsOverrideForTesting* GetVulkanScriptedTimestampValidBitsOverrideForTesting() noexcept {
    return g_scripted_timestamp_valid_bits_override.load(std::memory_order_acquire);
}

const VulkanScriptedPresentOverrideForTesting* GetVulkanScriptedPresentOverrideForTesting() noexcept {
    return g_scripted_present_override.load(std::memory_order_acquire);
}

bool TryLatchVulkanDeviceFaultForTesting(VkResult _result) noexcept {
    try {
        auto* device = static_cast<VulkanDevice*>(
            RenderDevice::Get().GetImpl()
        );
        if (device == nullptr) {
            return false;
        }
        const VulkanOperationContext context{
            .operation  = EVulkanFaultOperation::PresentSubmit,
            .queue_type = EQueueType::Graphics,
        };
        return device->TryLatchFirstFault(
                   context, _result, true, false, true
               ) ||
               device->IsFaulted();
    } catch (...) {
        return false;
    }
}

} // namespace Moer::Render
