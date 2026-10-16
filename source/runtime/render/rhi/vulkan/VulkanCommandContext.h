#pragma once

#include "VulkanCommand.h"
#include "VulkanCommandQueries.h"
#include "VulkanResourceTracker.h"
#include "VulkanTemporaryBufferAllocator.h"

#include <functional>
#include <optional>
#include <string_view>
#include <utility>

namespace Moer::Render {

// Shared command resources for ordinary recordings and presentation copies.
class VulkanCommandContextBase : public VulkanDeviceObject {
public:
    virtual ~VulkanCommandContextBase();
    VulkanCommandContextBase(const VulkanCommandContextBase&)            = delete;
    VulkanCommandContextBase& operator=(const VulkanCommandContextBase&) = delete;
    VulkanCommandContextBase(VulkanCommandContextBase&&)                 = delete;
    VulkanCommandContextBase& operator=(VulkanCommandContextBase&&)      = delete;

    VulkanCmdList& GetCmdList() {
        return cmd_list;
    }
    // Secondary allocation is deliberately explicit. Call this on the
    // coordinator before publishing work to a record worker: Vulkan command
    // pools require external synchronization even when different command
    // buffers are allocated from the pool.
    VulkanCmdList& PrepareSecondaryCmdList();
    VulkanCmdList& GetSecondaryCmdList() {
        return secondary_cmd_list.value();
    }
    [[nodiscard]] bool HasSecondaryCmdList() const noexcept {
        return secondary_cmd_list.has_value();
    }
    VkTracker& GetTracker() {
        return tracker;
    }
    void AddOnComplete(std::function<void()>&& _func) {
        on_complete.push_back(std::move(_func));
    }

    // Completion callbacks are user-extensible and must never escape the
    // Completion owner. A false result quarantines the context instead of
    // resetting/reusing partially finalized state.
    virtual bool CompleteSuccess() noexcept;
    virtual bool Reset();
    // A recorder that was never submitted must not run success callbacks.
    // It can still reset and return to the pool after all worker jobs joined.
    bool ResetAbandoned();

protected:
    VulkanCommandContextBase(VulkanDevice* _device, EQueueType _queue_type);

    bool ResetCommandPool();

    // Keep callbacks and tracker state alive until command resources are destroyed.
    EQueueType                   queue_type{EQueueType::Ignore};
    VkTracker                    tracker;
    Array<std::function<void()>> on_complete;

    // Reverse destruction releases both command buffers before their pool.
    VulkanCommandPool           command_pool;
    VulkanCmdList               cmd_list;
    std::optional<VulkanCmdList> secondary_cmd_list;
};

// A recording and its resources travel together through Submission and Completion.
class VulkanCommandContext : public VulkanCommandContextBase {
public:
    VulkanCommandContext(VulkanDevice* _device, EQueueType _queue_type);
    ~VulkanCommandContext() override = default;

    VulkanTemporaryBufferAllocator& GetTemporaryBufferAllocator() {
        return temporary_buffer_allocator;
    }

    // Query recording. Slots and tokens belong to this context's recording.
    void EnsureQueryCapacity(size_t _timestamp_slot_count, size_t _occlusion_pair_count);
    void
    ConfigureTimestampCapabilityForRecording(uint32 _valid_bits, EVulkanTimestampQueryResetMode _reset_mode);
    void RecordQuery(VulkanCmdList& _cmd_list, const QueryCmd& _cmd);

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

    bool CompleteSuccess() noexcept override;
    bool Reset() override;

private:
    VulkanTemporaryBufferAllocator temporary_buffer_allocator;
    VulkanCommandQueries           queries;
};

// Presentation copies share command resources and own an image-acquire semaphore.
class VulkanPresentContext : public VulkanCommandContextBase {
public:
    VulkanPresentContext(VulkanDevice* _device, EQueueType _queue_type);
    ~VulkanPresentContext() override;

    [[nodiscard]] VkSemaphore GetImageReadySemaphore() const noexcept {
        return image_ready_semaphore;
    }

    bool CompleteSuccess() noexcept override;

private:
    VkSemaphore image_ready_semaphore{VK_NULL_HANDLE};
};

} // namespace Moer::Render
