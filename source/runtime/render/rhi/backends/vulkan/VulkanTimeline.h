#pragma once

#include "VulkanDeviceObject.h"
#include "VulkanFault.h"
#include "misc/Types.h"
#include "rhi/RHIResource.h"

#include <atomic>
#include <condition_variable>
#include <mutex>

namespace Moer::Render {

// GPU signal progress backed by a VkSemaphore. CPU callbacks, query publication
// and resource retirement are tracked separately by the owning queue.
class VulkanTimeline final : public Fence, VulkanDeviceObject {
public:
    VulkanTimeline(VulkanDevice&);
    ~VulkanTimeline() override;

    // Last observed GPU value; may lag the semaphore. This watermark alone
    // does not imply that an individual value was accepted (see IsRejected).
    uint64 GetValue() const override;

    // Non-blocking: cached success, otherwise query Vulkan. Returns SUCCESS,
    // NOT_READY or an error; rejected values never report success.
    RENDER_API VkResult QueryGpuCompletion(uint64 value);

    // Wait for GPU completion, not the queue's CPU retirement work.
    RENDER_API bool Wait(uint64 value) override;

    // Only after native completion has been established, including an external
    // producer such as CUDA. Does not signal the semaphore or accept a submit.
    void RecordGpuCompletion(uint64 value);

    // Native-submission acceptance is independent of the GPU progress cache.
    void            MarkSubmitted(uint64 value) override;
    RENDER_API bool WaitSubmitted(
        uint64                  value,
        const std::atomic_bool* continue_waiting = nullptr
    ) override;
    void            Reject(uint64 value) noexcept override;
    RENDER_API bool IsRejected(uint64 value) const override;

    RENDER_API void Fail(VkResult result);
    RENDER_API bool IsFailed() const;
    VkSemaphore     GetSemaphore() const noexcept {
        return m_timeline;
    }

private:
    VkResult GetCachedGpuCompletionStatus(uint64 value) const;
    VkResult RefreshGpuCompletedValue(uint64 target_value);
    VkResult HandleGpuOperationFailure(VkResult result, uint64 value, EVulkanFaultOperation operation);

    VkSemaphore             m_timeline{VK_NULL_HANDLE};
    mutable std::mutex      m_state_mutex;
    uint64                  m_cached_gpu_completed_value{0};
    std::condition_variable m_submission_cv;
    uint64                  m_submitted_value{0};
    UnorderedSet<uint64>     m_rejected_values;
    bool                    m_is_failed{false};
    VkResult                m_failure_result{VK_SUCCESS};
};

using VulkanTimelineRef = CountableRef<VulkanTimeline>;

inline VulkanTimeline* ResourceCast(Fence* resource) {
    return static_cast<VulkanTimeline*>(resource);
}

} // namespace Moer::Render
