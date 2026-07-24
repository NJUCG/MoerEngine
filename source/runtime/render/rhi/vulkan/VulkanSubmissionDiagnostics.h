#pragma once

#include "RenderAPI.h"
#include "rhi/RHICommon.h"

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

} // namespace Moer::Render
