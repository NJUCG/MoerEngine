#pragma once

#include "misc/STL.h"
#include "rhi/RHIResource.h"

#include <string>

namespace Moer::Render {

class VulkanDevice;
class VulkanBuffer;

enum class EVkInternalBufferUsage {
    Upload,
    Readback,
    Scratch,
    ShaderBuffer,
    ShaderBuffer_Constant
};

// Buffers used by one command recording. The owner decides when GPU use has ended.
class VulkanTemporaryBufferAllocator {
public:
    explicit VulkanTemporaryBufferAllocator(VulkanDevice& _device);
    ~VulkanTemporaryBufferAllocator();
    VulkanTemporaryBufferAllocator(const VulkanTemporaryBufferAllocator&)            = delete;
    VulkanTemporaryBufferAllocator& operator=(const VulkanTemporaryBufferAllocator&) = delete;
    VulkanTemporaryBufferAllocator(VulkanTemporaryBufferAllocator&&)                 = delete;
    VulkanTemporaryBufferAllocator& operator=(VulkanTemporaryBufferAllocator&&)      = delete;

    BufferView AllocateUploadBuffer(uint64 _size, uint _alignment);
    BufferView AllocateReadbackBuffer(uint64 _size, uint _alignment);
    BufferView AllocateScratch(uint64 _size);
    BufferView AllocateShaderBuffer(uint64 _size);

    // Call only after GPU completion or abandonment of an unsubmitted recording.
    void Reset();

private:
    static constexpr uint64 small_block_size = 64 * 1024;
    static constexpr uint64 max_block_size   = 1024 * 1024;

    struct LinearBufferAllocator {
        struct BufferSlice {
            VulkanBuffer* buffer;
            uint64        byte_offset;
        };

        LinearBufferAllocator(
            VulkanDevice&          _device,
            EVkInternalBufferUsage _buffer_usage,
            uint64                 _initial_capacity,
            double                 _growth_factor
        );
        BufferSlice Allocate(uint64 _size, uint _alignment);
        void        Reset();
        void        Dispose();

    private:
        struct BufferBlock {
            VulkanBuffer* buffer;
            uint64        size_bytes;
            uint64        used_bytes;
        };

        VulkanBuffer* CreateBufferBlock(uint64 _size);
        std::string   MakeBufferName();

        VulkanDevice&          device;
        EVkInternalBufferUsage buffer_usage;
        Array<BufferBlock>     allocated_buffers;
        uint64                 next_block_capacity;
        uint64                 next_buffer_name_id{0};
        double                 growth_factor;
    };

    void ReleaseTemporaryBuffers();

    VulkanDevice& device;

    // Linear blocks are retained for reuse across safe recording resets.
    LinearBufferAllocator upload_allocator;
    LinearBufferAllocator readback_allocator;

    // Independent buffers are released together when this recording retires.
    Array<VulkanBuffer*> temporary_buffers;
};

} // namespace Moer::Render
