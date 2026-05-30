#pragma once
#include "RHICmdReorderer.h"
#include "VulkanCommand.h"
#include "VulkanMacroUtils.h"
#include "VulkanRHIResource.h"
#include "VulkanResourceTracker.h"
#include "rhi/RHICommon.h"
#include "string/StringConvert.h"
#include "vulkan/vulkan_core.h"

namespace Moer::Render {

enum class EVulkanSubmitPayloadType : uint8;
struct VulkanSubmitPayload;

class VulkanCmdAllocator : public VulkanDeviceObject {
private:
    VkQueueFlags  queue_type;
    VkCommandPool command_pool;

public:
    VulkanCmdAllocator(VulkanDevice* _device, EQueueType _queue_type);
    ~VulkanCmdAllocator();
    VkCommandPool GetHandle() const {
        return command_pool;
    }
    VkQueueFlags GetQueueType() const {
        return queue_type;
    }
    String GetQueueName() const {
        return Utf8ToPlatform(Utf8StringView(VK_TYPE_TO_STRING(VkQueueFlagBits, (VkQueueFlagBits)queue_type)));
    }
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
    uint64 Allocate(uint64 _size, StringView _name);
    uint64 Allocate(uint64 _size, EVkInternalBufferUsage _usage);
    void   DeAllocate(uint64 _handle);
};

//TODO: need refactor
// All allocator should be managed by device
// this VulkanAllocator should only have reference to some manager owned by device
// like stagingbuffer manager, querypool manager, command pool manager, etc
class VulkanAllocatorBase : public VulkanDeviceObject {
public:
    VulkanAllocatorBase(VulkanDevice* _device, EQueueType _queue_type);
    virtual ~VulkanAllocatorBase();

    VulkanCmdList& GetCmdList() {
        return active_cmd_list != nullptr ? *active_cmd_list : cmd_list.value();
    }
    VulkanSubmitPayload& BeginPayload(EVulkanSubmitPayloadType payload_type, EQueueType queue_type);
    VulkanSubmitPayload& GetCurrentPayload(EVulkanSubmitPayloadType payload_type, EQueueType queue_type);
    VulkanCmdList& BeginSubmitContext();
    void EndSubmitContext();
    void AddSubmitWait(WaitEvent event, EVulkanSubmitPayloadType payload_type, EQueueType queue_type);
    void AddSubmitSignal(SignalEvent event, EVulkanSubmitPayloadType payload_type, EQueueType queue_type);
    Array<UniquePtr<VulkanSubmitPayload>> ReleasePendingPayloads();
    VkTracker& GetTracker() {
        return tracker;
    }
    void ResetCmdList();
    void AddOnComplete(std::function<void()>&& _func) {
        submit_on_complete.push_back(std::move(_func));
    }
    void AddCompletionEvent(GraphEventRef _event) {
        if (_event) {
            submit_completion_events.push_back(std::move(_event));
        }
    }

    virtual void Complete(VulkanFence* _fence, uint64 _timeline);
    virtual void Reset();

protected:
    EQueueType                         allocator_queue_type{EQueueType::Ignore};
    std::optional<VulkanCmdAllocator> cmd_allocator;
    std::optional<VulkanCmdList>      cmd_list;
    Array<UniquePtr<VulkanCmdList>>   extra_cmd_lists{};
    VulkanCmdList*                    active_cmd_list{nullptr};
    uint32                            next_cmd_list_index{0};
    UniquePtr<VulkanSubmitPayload>    current_payload{};
    Array<UniquePtr<VulkanSubmitPayload>> pending_payloads{};

    Array<std::function<void()>> on_complete;
    Array<GraphEventRef>         completion_events;
    Array<std::function<void()>> submit_on_complete;
    Array<GraphEventRef>         submit_completion_events;

    VkTracker tracker;

private:
    VulkanSubmitPayload& GetOrCreateCurrentPayload(EVulkanSubmitPayloadType payload_type, EQueueType queue_type);
    void CloseCurrentPayload();
    bool CurrentPayloadHasWork() const;
};

class VkNativeQueryPool {
public:
    VkNativeQueryPool(VulkanDevice& _device, VkQueryType _type, uint32 _count);
    ~VkNativeQueryPool();
    VkQueryPool GetHandle() const {
        return query_pool;
    }
    uint32 GetCount() const {
        return count;
    }
    void
    GetResults(std::span<uint64> _results, uint32 _first_query, uint32 _query_cnt, VkQueryResultFlags _flags);
    VulkanDevice& GetDevice() const {
        return device;
    }

private:
    VulkanDevice& device;
    VkQueryPool   query_pool{VK_NULL_HANDLE};
    uint32        count;
    VkQueryType   type;
};

class VulkanPresentor : public VulkanAllocatorBase {
public:
    VulkanPresentor(VulkanDevice* _device, EQueueType _queue_type);
    virtual ~VulkanPresentor();

    void Complete(VulkanFence* _fence, uint64 _timeline) override;
};

class VulkanAllocator : public VulkanAllocatorBase {
    constexpr static uint64_t small_block_size = 64 * 1024;

public:
    VulkanAllocator(VulkanDevice* _device, EQueueType _queue_type);
    virtual ~VulkanAllocator();
    BufferView AllocateUploadBuffer(uint64 _size, uint _align);
    BufferView AllocateReadbackBuffer(uint64 _size, uint _align);

    BufferView AllocateScratch(uint64 _size);
    BufferView AllocateShaderBuffer(uint64 _size);

    void ResetBufferAlloc();

    void Complete(VulkanFence* _fence, uint64 _timeline) override;
    void Reset() override;
    //staging buffer allocate with block strategy
private:
    struct ScratchAllocator : VulkanDeviceObject {
        ScratchAllocator(VulkanDevice* _device, VkTmpBufferAllocator* _allocator);
        uint64 Allocate(uint64 _size);
        void   Deallocate(uint64 _handle);
        void   Reset();

        Array<uint64>         allocated_buffers;
        uint64                alignment;
        VkTmpBufferAllocator* allocator = nullptr;
    };

    struct ShaderBufferAllocator : VulkanDeviceObject {
        ShaderBufferAllocator(VulkanDevice* _device, VkTmpBufferAllocator* _allocator);
        uint64 Allocate(uint64 _size);
        void   Deallocate(uint64 _handle);
        void   Reset();

        Array<uint64>         allocated_buffers;
        uint64                alignment;
        VkTmpBufferAllocator* allocator = nullptr;
    };
    struct StackAllocator {
        uint64 init_capacity;
        uint64 capacity;
        uint64 stack_memory_id;
        double growth_factor;
        struct Chunk {
            uint64 handle;
            uint64 offset;
        };
        struct Buffer {
            uint64 handle;
            uint64 size;
            uint64 offset;
        };
        StackAllocator(VkTmpBufferAllocator*, uint64 _init_capacity, double _growth_factor);

        VkTmpBufferAllocator* allocator;
        Array<Buffer>         allocated_buffers;
        Chunk                 Allocate(uint64 _size, uint _align);
        Chunk                 Allocate(uint64 _size);
        void                  Reset();
        void                  Dispose();
        String                GetStackBufferName();
    };
    Array<VulkanBuffer*> large_buffers;
    VkTmpBufferAllocator allocator;

    StackAllocator upload_allocator;
    StackAllocator readback_allocator;

    ScratchAllocator      scratch_allocator;
    ShaderBufferAllocator shader_buffer_allocator;
};
} // namespace Moer::Render
