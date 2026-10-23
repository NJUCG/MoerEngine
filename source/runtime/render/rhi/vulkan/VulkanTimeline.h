#pragma once

#include "VulkanDeviceObject.h"
#include "VulkanFault.h"
#include "rhi/RHIResource.h"

#include <atomic>
#include <condition_variable>
#include <mutex>

namespace Moer::Render {

// RHI synchronization timeline backed by a VkSemaphore and host-side
// submission/completion state.
class VulkanTimeline final : public Fence, VulkanDeviceObject {
public:
    VulkanTimeline(VulkanDevice&);
    virtual ~VulkanTimeline();

    uint64_t GetValue() const override;

    uint64 GetDeviceValue() const;

    void Wait(uint64_t _value) override;
    void Reject(uint64_t _value) noexcept override;
    // can be called on any thread to block current thread
    void Sync(uint64);
    // Publish observed completion to CPU waiters; does not signal the semaphore.
    void            Notify(uint64);
    void            MarkSubmitted(uint64_t _value) override;
    RENDER_API bool WaitSubmitted(
        uint64_t                _value,
        const std::atomic_bool* _continue_waiting = nullptr,
        EQueueType              _waiting_queue    = EQueueType::Ignore,
        uint32                  _dependency_count = 0
    ) override;
    RENDER_API bool IsRejected(uint64_t _value) const override;
    RENDER_API VkResult
    HostWait(uint64_t _value, const VulkanOperationContext& _context = VulkanOperationContext{});
    RENDER_API void Fail(VkResult _result);
    RENDER_API bool IsFailed() const;
    RENDER_API void SignalHost(uint64_t _value);
    VkSemaphore     GetSemaphore() const noexcept {
        return timeline;
    }

private:
    uint64                  current_value = 0;
    VkSemaphore             timeline;
    std::condition_variable cv;
    mutable std::mutex      cv_m;
    uint64                  submitted_value{0};
    UnorderedSet<uint64>    rejected_values;
    bool                    failed{false};
    VkResult                failure_result{VK_SUCCESS};
};

using VulkanTimelineRef = CountableRef<VulkanTimeline>;

inline VulkanTimeline* ResourceCast(Fence* resource) {
    return static_cast<VulkanTimeline*>(resource);
}

} // namespace Moer::Render
