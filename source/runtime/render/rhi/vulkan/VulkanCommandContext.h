#pragma once

#include "VulkanCommandList.h"
#include "VulkanCommandQueries.h"
#include "VulkanResourceTracker.h"
#include "VulkanTemporaryBufferAllocator.h"

#include <functional>
#include <optional>
#include <string_view>
#include <utility>

namespace Moer::Render {

// A recording and its resources travel together through Submission and Completion.
class VulkanCommandContext : public VulkanDeviceObject {
public:
    VulkanCommandContext(VulkanDevice* _device, EQueueType _queue_type);
    ~VulkanCommandContext();
    VulkanCommandContext(const VulkanCommandContext&)            = delete;
    VulkanCommandContext& operator=(const VulkanCommandContext&) = delete;
    VulkanCommandContext(VulkanCommandContext&&)                 = delete;
    VulkanCommandContext& operator=(VulkanCommandContext&&)      = delete;

    VulkanCommandList& GetCommandList() {
        return cmd_list;
    }
    // Secondary allocation is deliberately explicit. Call this on the
    // coordinator before publishing work to a record worker: Vulkan command
    // pools require external synchronization even when different command
    // buffers are allocated from the pool.
    VulkanCommandList& PrepareSecondaryCommandList();
    VulkanCommandList& GetSecondaryCommandList() {
        return secondary_cmd_list.value();
    }
    [[nodiscard]] bool HasSecondaryCommandList() const noexcept {
        return secondary_cmd_list.has_value();
    }
    VulkanResourceTracker& GetTracker() {
        return tracker;
    }
    void AddOnComplete(std::function<void()>&& _func) {
        on_complete.push_back(std::move(_func));
    }

    VulkanTemporaryBufferAllocator& GetTemporaryBufferAllocator() {
        if (!temporary_buffer_allocator) {
            temporary_buffer_allocator = MakeUnique<VulkanTemporaryBufferAllocator>(*m_device);
        }
        return *temporary_buffer_allocator;
    }

    // Query recording. Slots and tokens belong to this context's recording.
    void EnsureQueryCapacity(size_t _timestamp_slot_count, size_t _occlusion_pair_count);
    void
    ConfigureTimestampCapabilityForRecording(uint32 _valid_bits, EVulkanTimestampQueryResetMode _reset_mode);
    void RecordQuery(VulkanCommandList& _cmd_list, const QueryCmd& _cmd);

    [[nodiscard]] bool HasQueries() const noexcept {
        return queries.HasQueries();
    }

    // Completion separates native readback, result publication, and callbacks.
    [[nodiscard]] VkResult PrepareQueriesAfterGpuCompletion(const VulkanOperationContext& _context) noexcept;

    bool CompleteSuccessCallbacks() noexcept;

    void PublishQueriesAfterGpuCompletion(
        QueryPublishBatch _batch,
        bool              _gpu_success,
        std::string_view  _failure_reason
    ) noexcept;
    void NotifyQueriesAfterGpuCompletion(QueryPublishBatch _batch) noexcept;

    bool                      CompleteSuccess() noexcept;
    bool                      Reset();
    bool                      ResetAbandoned();
    [[nodiscard]] VkSemaphore GetImageReadySemaphore();
    void                      MarkImageAcquired() noexcept {
        image_acquisition_pending = true;
    }

private:
    bool ResetCommandPool();

    // Release command buffers before the pool, callbacks and tracker.
    EQueueType                                queue_type{EQueueType::Ignore};
    VulkanResourceTracker                     tracker;
    Array<std::function<void()>>              on_complete;
    VulkanCommandPool                         command_pool;
    VulkanCommandList                         cmd_list;
    std::optional<VulkanCommandList>          secondary_cmd_list;
    UniquePtr<VulkanTemporaryBufferAllocator> temporary_buffer_allocator;
    VulkanCommandQueries                      queries;
    VkSemaphore                               image_ready_semaphore{VK_NULL_HANDLE};
    bool                                      image_acquisition_pending{false};
};

} // namespace Moer::Render
