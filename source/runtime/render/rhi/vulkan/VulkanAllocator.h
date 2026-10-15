#pragma once
#include "VulkanCommand.h"
#include "VulkanCommandQueries.h"
#include "VulkanCommon.h"
#include "VulkanRHIResource.h"
#include "VulkanResourceTracker.h"
#include "misc/STL.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIQuery.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace Moer::Render {

class VulkanCommandPool : public VulkanDeviceObject {
public:
    VulkanCommandPool(VulkanDevice* _device, EQueueType _queue_type);
    ~VulkanCommandPool();
    VulkanCommandPool(const VulkanCommandPool&)            = delete;
    VulkanCommandPool& operator=(const VulkanCommandPool&) = delete;
    VulkanCommandPool(VulkanCommandPool&&)                 = delete;
    VulkanCommandPool& operator=(VulkanCommandPool&&)      = delete;

    VkCommandPool GetHandle() const {
        return command_pool;
    }
    VkQueueFlags GetQueueFlags() const {
        return queue_flags;
    }
    std::string_view GetQueueName() const;

private:
    VkQueueFlags  queue_flags;
    VkCommandPool command_pool{VK_NULL_HANDLE};
};

enum class EVkInternalBufferUsage {
    Upload,
    Readback,
    Scratch,
    ShaderBuffer,
    ShaderBuffer_Constant
};
struct VkTmpBufferAllocator : VulkanDeviceObject {
    VkTmpBufferAllocator(VulkanDevice* _device);
    uint64 Allocate(uint64 _size, std::string_view _name);
    uint64 Allocate(uint64 _size, EVkInternalBufferUsage _usage);
    void   DeAllocate(uint64 _handle);
};

class VulkanAllocatorBase : public VulkanDeviceObject {
public:
    VulkanAllocatorBase(VulkanDevice* _device, EQueueType _queue_type);
    virtual ~VulkanAllocatorBase();
    VulkanAllocatorBase(const VulkanAllocatorBase&)            = delete;
    VulkanAllocatorBase& operator=(const VulkanAllocatorBase&) = delete;
    VulkanAllocatorBase(VulkanAllocatorBase&&)                 = delete;
    VulkanAllocatorBase& operator=(VulkanAllocatorBase&&)      = delete;

    VulkanCmdList& GetCmdList() {
        return cmd_list.value();
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
    bool ResetCmdList();
    void AddOnComplete(std::function<void()>&& _func) {
        on_complete.push_back(std::move(_func));
    }

    // Completion callbacks are user-extensible and must never escape the
    // Completion owner. A false result quarantines the allocator instead of
    // resetting/reusing partially finalized state.
    virtual bool CompleteSuccess() noexcept;
    virtual bool Reset();
    // A recorder that was never submitted must not run success callbacks.
    // It can still reset and return to the pool after all worker jobs joined.
    bool ResetAbandoned();

protected:
    std::optional<VulkanCommandPool> command_pool;
    std::optional<VulkanCmdList>     cmd_list;
    std::optional<VulkanCmdList>     secondary_cmd_list;

    Array<std::function<void()>> on_complete;

    EQueueType queue_type{EQueueType::Ignore};
    VkTracker  tracker;
};

class VulkanPresentor : public VulkanAllocatorBase {
public:
    VulkanPresentor(VulkanDevice* _device, EQueueType _queue_type);
    ~VulkanPresentor() override;

    [[nodiscard]] VkSemaphore GetImageReadySemaphore() const noexcept {
        return image_ready_semaphore;
    }

    bool CompleteSuccess() noexcept override;

private:
    VkSemaphore image_ready_semaphore{VK_NULL_HANDLE};
};

class VulkanAllocator : public VulkanAllocatorBase {
public:
    VulkanAllocator(VulkanDevice* _device, EQueueType _queue_type);
    ~VulkanAllocator() override;

    BufferView AllocateUploadBuffer(uint64 _size, uint _align);
    BufferView AllocateReadbackBuffer(uint64 _size, uint _align);

    BufferView AllocateScratch(uint64 _size);
    BufferView AllocateShaderBuffer(uint64 _size);

    void ResetBufferAlloc();

    // Query recording. Slots and tokens belong to this allocator's recording.
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
    static constexpr uint64 small_block_size = 64 * 1024;

    // Temporary buffer allocation strategies.
    struct LinearBufferAllocator {
        struct BufferSlice {
            uint64 handle;
            uint64 byte_offset;
        };

        LinearBufferAllocator(
            VkTmpBufferAllocator* _allocator,
            uint64                _initial_capacity,
            double                _growth_factor
        );
        BufferSlice Allocate(uint64 _size, uint _alignment);
        void        Reset();
        void        Dispose();

    private:
        struct BufferBlock {
            uint64 handle;
            uint64 size_bytes;
            uint64 used_bytes;
        };

        std::string MakeBufferName();

        VkTmpBufferAllocator* allocator;
        Array<BufferBlock>    allocated_buffers;
        uint64                next_block_capacity;
        uint64                next_buffer_name_id{0};
        double                growth_factor;
    };

    template<EVkInternalBufferUsage Usage>
    class DedicatedBufferAllocator {
    public:
        explicit DedicatedBufferAllocator(VkTmpBufferAllocator& _allocator) : allocator(_allocator) {}

        uint64 Allocate(uint64 _size) {
            const uint64 handle = allocator.Allocate(_size, Usage);
            allocated_buffers.push_back(handle);
            return handle;
        }

        void Reset() {
            for (uint64 handle : allocated_buffers) {
                allocator.DeAllocate(handle);
            }
            allocated_buffers.clear();
        }

    private:
        VkTmpBufferAllocator& allocator;
        Array<uint64>         allocated_buffers;
    };

    // Temporary buffers live until this command recording is retired.
    VkTmpBufferAllocator allocator;
    Array<VulkanBuffer*> large_buffers;

    LinearBufferAllocator upload_allocator;
    LinearBufferAllocator readback_allocator;

    DedicatedBufferAllocator<EVkInternalBufferUsage::Scratch>      scratch_allocator;
    DedicatedBufferAllocator<EVkInternalBufferUsage::ShaderBuffer> shader_buffer_allocator;

    // Query state follows the lifetime of this command recording.
    VulkanCommandQueries queries;
};
} // namespace Moer::Render
