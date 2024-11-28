#pragma once
#include "RHICmdReorderer.h"
#include "VulkanRHIResource.h"
#include "VulkanResourceTracker.h"
#include "VulkanCommand.h"
#include "rhi/RHICommon.h"

namespace Moer::Render {

    class VulkanCmdAllocator : public VulkanDeviceObject {
    private:
        VkQueueFlags  queue_type;
        VkCommandPool command_pool;

    public:
        VulkanCmdAllocator(VulkanDevice* _device, VkQueueFlagBits _queue_type);
        ~VulkanCmdAllocator();
        VkCommandPool GetHandle() const { return command_pool; }
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
        uint64 Allocate(uint64 _size);
        uint64 Allocate(uint64 _size, EVkInternalBufferUsage _usage);
        void   DeAllocate(uint64 _handle);
    };

    class VulkanAllocator : public VulkanDeviceObject {
        constexpr static uint64_t small_block_size = 64 * 1024;

    public:
        VulkanAllocator(VulkanDevice* _device, EQueueType _queue_type);
        ~VulkanAllocator();
        BufferView AllocateUploadBuffer(uint64 _size, uint _align);
        BufferView AllocateReadbackBuffer(uint64 _size, uint _align);

        BufferView     AllocateScratch(uint64 _size);
        BufferView     AllocateShaderBuffer(uint64 _size);
        VulkanCmdList& GetCmdList() {
            return cmd_list.value();
        }
        VkTracker& GetTracker() {
            return tracker;
        }
        void ResetBufferAlloc();
        void ResetCmdList();
        void Complete(VulkanFence* _fence, uint64 _timeline);
        void Reset();
        void AddOnComplete(std::function<void()>&& _func) {
            on_complete.push_back(std::move(_func));
        }
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
        };
        std::optional<VulkanCmdAllocator> cmd_allocator;
        std::optional<VulkanCmdList>      cmd_list;
        Array<VulkanBuffer*>              large_buffers;
        VkTmpBufferAllocator              allocator;

        StackAllocator upload_allocator;
        StackAllocator readback_allocator;

        ScratchAllocator             scratch_allocator;
        ShaderBufferAllocator        shader_buffer_allocator;
        Array<std::function<void()>> on_complete;
        VkTracker                    tracker;
    };
}// namespace Moer::Render