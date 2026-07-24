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
    uint32     source_index{0};
    EQueueType queue{EQueueType::Ignore};
    uint64     async_queue_scope{0};
};

using VulkanSourceSubmissionCallback = void (*)(void*, const VulkanSourceSubmissionEvent&) noexcept;

struct VulkanSourceSubmissionObserver {
    void*                          context{nullptr};
    VulkanSourceSubmissionCallback callback{nullptr};
};

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

// Backend-internal notification points shared across Vulkan translation,
// resource, and submission implementation units.
void NotifyVulkanSubmissionDependencyWaitBlocked(
    EQueueType _queue,
    uint32     _dependency_count
) noexcept;
void NotifyVulkanBackendSyncWait() noexcept;

} // namespace Moer::Render
