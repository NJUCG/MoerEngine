#pragma once

#include "RenderAPI.h"
#include "VulkanFault.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIThreadOwnership.h"

namespace Moer::Render {

// A successful source-packet handoff observed on the sole Vulkan Submission
// owner. This is intentionally below Translate and above native completion, so
// diagnostics can distinguish source order from worker completion order.
struct VulkanSourceSubmissionEvent {
    uint64     batch_sequence{0};
    // Stable executable-entry index after backend segment materialization.
    uint32     source_index{0};
    // Original upper source identity remains stable when one source lowers to
    // several executable segment packets.
    uint32     original_source_index{0};
    uint32     source_segment_index{0};
    uint32     source_segment_count{1};
    EQueueType queue{EQueueType::Ignore};
    uint64     async_queue_scope{0};
    bool       cross_native_predecessor_wait{false};
};

using VulkanSourceSubmissionCallback = void (*)(void*, const VulkanSourceSubmissionEvent&) noexcept;

struct VulkanSourceSubmissionObserver {
    void*                          context{nullptr};
    VulkanSourceSubmissionCallback callback{nullptr};
};

enum class EVulkanSubmissionBoundaryKind : uint8_t {
    SerialControl = 0,
    PresentBridge,
    Present,
};

enum class EVulkanSubmissionBoundaryPhase : uint8_t {
    Dispatch = 0,
    Terminal,
};

// Read-only observation of the stable Vulkan Submission cursor. Source
// operations use their executable source index. PresentBridge reserves the
// slot immediately after the source suffix and Present uses the following
// slot even when no bridge is needed, so operation identity does not depend
// on runtime queue aliasing or frontier state.
struct VulkanSubmissionBoundaryEvent {
    uint64                        batch_sequence{0};
    uint32                        operation_index{0};
    EQueueType                    queue{EQueueType::Ignore};
    EVulkanSubmissionBoundaryKind kind{
        EVulkanSubmissionBoundaryKind::SerialControl
    };
    EVulkanSubmissionBoundaryPhase phase{
        EVulkanSubmissionBoundaryPhase::Dispatch
    };
    uint32                        dependency_wait_count{0};
    uint32                        thread_id{0};
    ERHIThreadRole                thread_role{ERHIThreadRole::Unknown};
    VulkanOperationResult         outcome{};
    bool                          outcome_valid{false};
    bool                          gpu_submitted{false};
    bool                          recoverable_rejection{false};
    uint32                        present_receipt_resolution_attempts{0};
};

using VulkanSubmissionBoundaryCallback =
    void (*)(void*, const VulkanSubmissionBoundaryEvent&) noexcept;

struct VulkanSubmissionBoundaryObserver {
    void*                            context{nullptr};
    VulkanSubmissionBoundaryCallback callback{nullptr};
};

// Callback storage is caller-owned and immutable while installed. Callbacks
// run on the sole Submission owner immediately before an ordered queue
// operation and again after it reaches a terminal runtime result. They must be
// short, non-blocking, noexcept, and must not re-enter RHI. Quiesce observed
// work before removal; removal does not wait for an already-loaded callback.
[[nodiscard]] RENDER_API bool
TryInstallVulkanSubmissionBoundaryObserver(
    const VulkanSubmissionBoundaryObserver* _observer
) noexcept;
[[nodiscard]] RENDER_API bool
RemoveVulkanSubmissionBoundaryObserver(
    const VulkanSubmissionBoundaryObserver* _observer
) noexcept;

enum class EVulkanSourceTranslationPhase : uint8_t {
    Begin = 0,
    Recorded,
    Failed,
};

// Observation around one explicit-RDG source's Translate operation. Recorded
// proves that the move-only packet has acquired its queue allocator/session
// state and is ready for the stable Submission cursor; it does not imply a
// native submit.
struct VulkanSourceTranslationEvent {
    uint64                        batch_sequence{0};
    uint32                        source_index{0};
    uint32                        original_source_index{0};
    uint32                        source_segment_index{0};
    uint32                        source_segment_count{1};
    EQueueType                    queue{EQueueType::Ignore};
    uint32                        native_queue_id{0};
    uint64                        async_queue_scope{0};
    uint32                        thread_id{0};
    ERHIThreadRole                thread_role{ERHIThreadRole::Unknown};
    bool                          parallel_record_requested{false};
    bool                          parallel_record_planned{false};
    bool                          parallel_record_effective{false};
    EVulkanSourceTranslationPhase phase{
        EVulkanSourceTranslationPhase::Begin
    };
};

using VulkanSourceTranslationCallback =
    void (*)(void*, const VulkanSourceTranslationEvent&) noexcept;

struct VulkanSourceTranslationObserver {
    void*                           context{nullptr};
    VulkanSourceTranslationCallback callback{nullptr};
};

// Narrow for-testing seam for the real multi-segment completion aggregate.
// It deliberately exposes only observable callback ordering/counts, not the
// backend-private aggregate or its synchronization storage.
struct VulkanMultiSegmentCompletionProbeResult {
    bool   suffix_retirement_deferred_callbacks{false};
    bool   prefix_retirement_completed_callbacks{false};
    bool   repeated_retirement_suppressed{false};
    uint32 ordinary_callback_count{0};
    uint32 success_callback_count{0};
    uint32 materialized_prefix_signal_count{0};
    uint32 materialized_prefix_keepalive_count{0};
    uint32 materialized_suffix_signal_count{0};
    uint32 materialized_suffix_keepalive_count{0};
    bool   materialized_signal_identity_matches{false};
    bool   source_signal_remained_unrejected{false};
};

[[nodiscard]] RENDER_API VulkanMultiSegmentCompletionProbeResult
RunVulkanMultiSegmentCompletionProbeForTesting();

// Observer storage is caller-owned and must remain immutable while installed.
// The callback runs on the Submission owner and must be short, non-blocking,
// noexcept, and must not re-enter RHI. Install before publishing the observed
// work, quiesce that work, remove the observer, and only then destroy it;
// removal does not wait for a callback which has already loaded the observer.
// Runtime cost while disabled is one atomic pointer load per submitted source.
[[nodiscard]] RENDER_API bool
TryInstallVulkanSourceSubmissionObserver(const VulkanSourceSubmissionObserver* _observer) noexcept;
[[nodiscard]] RENDER_API bool
RemoveVulkanSourceSubmissionObserver(const VulkanSourceSubmissionObserver* _observer) noexcept;

// Observer storage is caller-owned and immutable while installed. The
// callback runs on a Translate owner and must be short, non-blocking, noexcept,
// and must not re-enter RHI. Quiesce the observed work before removal.
[[nodiscard]] RENDER_API bool
TryInstallVulkanSourceTranslationObserver(
    const VulkanSourceTranslationObserver* _observer
) noexcept;
[[nodiscard]] RENDER_API bool
RemoveVulkanSourceTranslationObserver(
    const VulkanSourceTranslationObserver* _observer
) noexcept;

struct VulkanBatchPreflightRejectionEvent {
    uint64         batch_sequence{0};
    uint32         thread_id{0};
    ERHIThreadRole thread_role{ERHIThreadRole::Unknown};
    bool           executable_preflight{false};
};

using VulkanBatchPreflightRejectionCallback =
    void (*)(void*, const VulkanBatchPreflightRejectionEvent&) noexcept;

struct VulkanBatchPreflightRejectionObserver {
    void*                                 context{nullptr};
    VulkanBatchPreflightRejectionCallback callback{nullptr};
};

// Narrow diagnostic seam reached after batch validation fails and before any
// accepted pipeline prefix is drained or the malformed batch is terminalized.
// The callback runs on the Translate owner and must not block or re-enter RHI.
// Observer storage is caller-owned and must remain immutable while installed.
// Quiesce Translate work before removal, then remove the observer before
// destroying it or its context; removal does not wait for a callback that has
// already loaded the observer. It is only queried on an already-failing
// preflight path.
[[nodiscard]] RENDER_API bool
TryInstallVulkanBatchPreflightRejectionObserver(
    const VulkanBatchPreflightRejectionObserver* _observer
) noexcept;
[[nodiscard]] RENDER_API bool
RemoveVulkanBatchPreflightRejectionObserver(
    const VulkanBatchPreflightRejectionObserver* _observer
) noexcept;

// A direct observation at the VkNativeQueue -> VulkanDevice submit boundary.
// Unlike the source-packet observer above, this proves which OS thread and RHI
// role actually invoked the native queue operation.
struct VulkanNativeSubmissionEvent {
    EQueueType              queue{EQueueType::Ignore};
    uint64                  native_queue_handle{0};
    uint32                  thread_id{0};
    ERHIThreadRole          thread_role{ERHIThreadRole::Unknown};
    VulkanOperationResult   outcome{};
    bool                    empty_submit{false};
};

using VulkanNativeSubmissionCallback =
    void (*)(void*, const VulkanNativeSubmissionEvent&) noexcept;

struct VulkanNativeSubmissionObserver {
    void*                          context{nullptr};
    VulkanNativeSubmissionCallback callback{nullptr};
};

// The callback runs immediately after the native submit call returns. The
// caller owns observer storage and must keep it immutable until removal.
[[nodiscard]] RENDER_API bool
TryInstallVulkanNativeSubmissionObserver(
    const VulkanNativeSubmissionObserver* _observer
) noexcept;
[[nodiscard]] RENDER_API bool
RemoveVulkanNativeSubmissionObserver(
    const VulkanNativeSubmissionObserver* _observer
) noexcept;

struct VulkanSubmissionDependencyWaitEvent {
    EQueueType     queue{EQueueType::Ignore};
    uint32         thread_id{0};
    ERHIThreadRole thread_role{ERHIThreadRole::Unknown};
    uint32         dependency_count{0};
};

using VulkanSubmissionDependencyWaitCallback =
    void (*)(void*, const VulkanSubmissionDependencyWaitEvent&) noexcept;

struct VulkanSubmissionDependencyWaitObserver {
    void*                                  context{nullptr};
    VulkanSubmissionDependencyWaitCallback callback{nullptr};
};

// Test/diagnostic seam on the first unsatisfied WaitSubmitted blocking cycle
// entered by the sole Submission owner. The callback runs while the fence wait
// mutex is held and therefore must not block or re-enter RHI. Disabled cost is
// one atomic pointer load for a dependency that actually blocks.
[[nodiscard]] RENDER_API bool
TryInstallVulkanSubmissionDependencyWaitObserver(
    const VulkanSubmissionDependencyWaitObserver* _observer
) noexcept;
[[nodiscard]] RENDER_API bool
RemoveVulkanSubmissionDependencyWaitObserver(
    const VulkanSubmissionDependencyWaitObserver* _observer
) noexcept;

struct VulkanBackendSyncWaitEvent {
    uint32         thread_id{0};
    ERHIThreadRole thread_role{ERHIThreadRole::Unknown};
};

using VulkanBackendSyncWaitCallback =
    void (*)(void*, const VulkanBackendSyncWaitEvent&) noexcept;

struct VulkanBackendSyncWaitObserver {
    void*                         context{nullptr};
    VulkanBackendSyncWaitCallback callback{nullptr};
};

// Fires after Vulkan's backend Sync request is owned by the runtime FIFO and
// immediately before its caller waits for completion. At this point the upper
// RHIExecutor has already registered the active Sync call.
[[nodiscard]] RENDER_API bool
TryInstallVulkanBackendSyncWaitObserver(
    const VulkanBackendSyncWaitObserver* _observer
) noexcept;
[[nodiscard]] RENDER_API bool
RemoveVulkanBackendSyncWaitObserver(
    const VulkanBackendSyncWaitObserver* _observer
) noexcept;

struct VulkanQueueLocalSyncWaitEvent {
    EQueueType     queue{EQueueType::Ignore};
    uint32         thread_id{0};
    ERHIThreadRole thread_role{ERHIThreadRole::Unknown};
    uint64         target_retirement_serial{0};
    uint32         completion_group_count{0};
};

using VulkanQueueLocalSyncWaitCallback =
    void (*)(
        void*,
        const VulkanQueueLocalSyncWaitEvent&
    ) noexcept;

struct VulkanQueueLocalSyncWaitObserver {
    void*                            context{nullptr};
    VulkanQueueLocalSyncWaitCallback callback{nullptr};
};

// Fires inside queue-local CompleteAll after it snapshots the target
// retirement serial and every associated batch Completion group, immediately
// before it waits. The callback runs while the queue event mutex is held and
// therefore must be non-blocking, noexcept, and must not re-enter RHI.
[[nodiscard]] RENDER_API bool
TryInstallVulkanQueueLocalSyncWaitObserver(
    const VulkanQueueLocalSyncWaitObserver* _observer
) noexcept;
[[nodiscard]] RENDER_API bool
RemoveVulkanQueueLocalSyncWaitObserver(
    const VulkanQueueLocalSyncWaitObserver* _observer
) noexcept;

struct VulkanQueueIdleWaitEvent {
    VulkanOperationContext context{};
    uint64                 native_queue_handle{0};
    uint32                 thread_id{0};
    ERHIThreadRole         thread_role{ERHIThreadRole::Unknown};
};

using VulkanQueueIdleWaitCallback =
    void (*)(void*, const VulkanQueueIdleWaitEvent&) noexcept;

struct VulkanQueueIdleWaitObserver {
    void*                       context{nullptr};
    VulkanQueueIdleWaitCallback callback{nullptr};
};

// Fires after Vulkan queue synchronization has been acquired and immediately
// before vkQueueWaitIdle. Presentation fallback tests use this seam to prove
// the native wait remains owned by the sole Submission thread.
[[nodiscard]] RENDER_API bool
TryInstallVulkanQueueIdleWaitObserver(
    const VulkanQueueIdleWaitObserver* _observer
) noexcept;
[[nodiscard]] RENDER_API bool
RemoveVulkanQueueIdleWaitObserver(
    const VulkanQueueIdleWaitObserver* _observer
) noexcept;

void NotifyVulkanQueueIdleWait(
    const VulkanOperationContext& _context
) noexcept;

using VulkanScriptedPresentFenceStatusCallback =
    VkResult (*)(void*, uint64) noexcept;

struct VulkanScriptedPresentFenceStatusOverrideForTesting {
    void*                                    context{nullptr};
    VulkanScriptedPresentFenceStatusCallback callback{nullptr};
};

// Deterministic WSI-fence observation seam. It is valid only for synthetic
// Presentation completion probes; production Present batches always query
// their native VkFence when no override is installed.
[[nodiscard]] RENDER_API bool
TryInstallVulkanScriptedPresentFenceStatusOverrideForTesting(
    const VulkanScriptedPresentFenceStatusOverrideForTesting* _override
) noexcept;
[[nodiscard]] RENDER_API bool
RemoveVulkanScriptedPresentFenceStatusOverrideForTesting(
    const VulkanScriptedPresentFenceStatusOverrideForTesting* _override
) noexcept;

using VulkanScriptedQueryPreparationCallback =
    VkResult (*)(void*, EQueueType, uint64, uint32) noexcept;

struct VulkanScriptedQueryPreparationOverrideForTesting {
    void*                                    context{nullptr};
    VulkanScriptedQueryPreparationCallback  callback{nullptr};
};

// Narrow deterministic seam for a recoverable timestamp-pool preparation
// rejection. The callback runs on the Translate owner while queue execution is
// serialized, after the logical timeline is reserved but before command-buffer
// recording or native query-pool allocation. VK_SUCCESS continues through the
// production path; a non-device-loss error rejects this source without latching
// the Vulkan device.
//
// Override storage and context are caller-owned and immutable while installed.
// Quiesce the RHI runtime before removal and destroy them only after removal.
[[nodiscard]] RENDER_API bool
TryInstallVulkanScriptedQueryPreparationOverrideForTesting(
    const VulkanScriptedQueryPreparationOverrideForTesting* _override
) noexcept;
[[nodiscard]] RENDER_API bool
RemoveVulkanScriptedQueryPreparationOverrideForTesting(
    const VulkanScriptedQueryPreparationOverrideForTesting* _override
) noexcept;

using VulkanScriptedTimestampValidBitsCallback =
    uint32 (*)(void*, EQueueType, uint32) noexcept;

struct VulkanScriptedTimestampValidBitsOverrideForTesting {
    void*                                    context{nullptr};
    VulkanScriptedTimestampValidBitsCallback callback{nullptr};
};

// Narrow Copy-query capability seam. The callback receives the native queue
// family value on the Translate owner and returns an upper bound for this
// recording. The runtime clamps it to the native value, so a test can degrade
// capability but can never manufacture unsupported hardware capability.
// Production has no installed override. Tests use zero to prove that
// timestamp observability degrades without rejecting real Copy work.
//
// Override storage and context are caller-owned and immutable while installed.
// Quiesce the RHI runtime before removal and destroy them only after removal.
[[nodiscard]] RENDER_API bool
TryInstallVulkanScriptedTimestampValidBitsOverrideForTesting(
    const VulkanScriptedTimestampValidBitsOverrideForTesting* _override
) noexcept;
[[nodiscard]] RENDER_API bool
RemoveVulkanScriptedTimestampValidBitsOverrideForTesting(
    const VulkanScriptedTimestampValidBitsOverrideForTesting* _override
) noexcept;

struct VulkanScriptedPresentResult {
    VulkanOperationResult outcome{
        EVulkanOperationStatus::Retry,
        VK_NOT_READY,
    };
};

using VulkanScriptedPresentCallback =
    VulkanScriptedPresentResult (*)(void*, uint64) noexcept;
using VulkanPresentSourceRejectionCallback =
    void (*)(void*) noexcept;

struct VulkanScriptedPresentOverrideForTesting {
    void*                           context{nullptr};
    VulkanScriptedPresentCallback  callback{nullptr};
    // Opt-in for source-state boundary tests. Ordinary scripted outcomes do
    // not record/copy an image and therefore need no native source layout.
    bool require_present_source_ready{false};
    // Optional race-injection point after the initial device-fault check has
    // passed but immediately before an invalid/not-ready source is
    // classified. This keeps fault-priority tests deterministic without
    // weakening the production source contract.
    VulkanPresentSourceRejectionCallback
        before_source_rejection{nullptr};
};

// Narrow deterministic test seam for no-GPU-tail Present outcomes. The
// callback runs on the Submission owner while the queue execution mutex and
// runtime execution lease are held. It must be short, non-blocking, noexcept,
// and must not re-enter RHI. It is queried only after the real owner has
// serialized execution and reserved the logical Present timeline. Success is
// deliberately forbidden: tests may script Retry, Recreate, or Rejected,
// while the production path remains the only route that can report a native
// Present success or device fault.
//
// Override storage and context are caller-owned and immutable while installed.
// Quiesce with Sync(Present) or shutdown before removal and destroy the
// override/context only after removal. Removal does not wait for a callback
// which has already loaded the pointer.
[[nodiscard]] RENDER_API bool
TryInstallVulkanScriptedPresentOverrideForTesting(
    const VulkanScriptedPresentOverrideForTesting* _override
) noexcept;
[[nodiscard]] RENDER_API bool
RemoveVulkanScriptedPresentOverrideForTesting(
    const VulkanScriptedPresentOverrideForTesting* _override
) noexcept;

// Terminal test seam used only by isolated fault-boundary executables. It
// publishes a synthetic first Vulkan fault without issuing a native call.
[[nodiscard]] RENDER_API bool
TryLatchVulkanDeviceFaultForTesting(VkResult _result) noexcept;

// Backend-internal notification points shared across Vulkan translation,
// resource, and submission implementation units.
void NotifyVulkanSubmissionDependencyWaitBlocked(
    EQueueType _queue,
    uint32     _dependency_count
) noexcept;
void NotifyVulkanBackendSyncWait() noexcept;

} // namespace Moer::Render
