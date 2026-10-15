#include "VulkanTemporaryBufferAllocator.h"
#include "VulkanDevice.h"
#include "VulkanMacroUtils.h"
#include "VulkanRHIResource.h"

#include <algorithm>
#include <format>
#include <string_view>

namespace Moer::Render {

// Keep these functions addressable and out of line for the optional profiler's
// PDB hooks. Their implementation stays local to this translation unit.
#if defined(_MSC_VER)
#define MOER_TEMP_BUFFER_NOINLINE __declspec(noinline)
#else
#define MOER_TEMP_BUFFER_NOINLINE __attribute__((noinline))
#endif

MOER_TEMP_BUFFER_NOINLINE VulkanBuffer*
CreateTemporaryBuffer(VulkanDevice& _device, uint64 _size, std::string_view _name) {
    VkBufferCreateInfo buffer_info = {
        .sType                 = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext                 = nullptr,
        .flags                 = 0,
        .size                  = _size,
        .usage                 = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode           = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices   = nullptr
    };

#if WITH_CUDA
    buffer_info.pNext = GetExternalMemoryBufferCreateInfoPtr(nullptr);
#endif

    VmaAllocationCreateInfo alloc_info{
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT, .usage = VMA_MEMORY_USAGE_AUTO
    };
    VulkanBuffer::BufferAlloc buffer_alloc;
    BufferInfo                info(_size, 1, EBufferUsageFlags::CPU_VISIBLE);

    VK_CHECK_RESULT(vmaCreateBuffer(
        _device.GetVmaAllocator(),
        &buffer_info,
        &alloc_info,
        &buffer_alloc.buffer,
        &buffer_alloc.alloc,
        nullptr
    ));
    VulkanBuffer* vk_buffer =
        MoerNew(VulkanBuffer)(_name, info, _device, buffer_alloc.buffer, buffer_alloc.alloc, false);

    return vk_buffer;
}

MOER_TEMP_BUFFER_NOINLINE VulkanBuffer*
CreateTemporaryBuffer(VulkanDevice& _device, uint64 _size, EVkInternalBufferUsage _usage) {

    VkBufferUsageFlags       usage{};
    VmaAllocationCreateFlags flags{};
    VmaMemoryUsage           mem_usage      = VMA_MEMORY_USAGE_AUTO;
    bool                     device_address = false;
    switch (_usage) {

        case EVkInternalBufferUsage::Upload:
        case EVkInternalBufferUsage::Readback: {
            usage     = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            flags     = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
            mem_usage = VMA_MEMORY_USAGE_AUTO;
            break;
        }
        case EVkInternalBufferUsage::Scratch: {
            usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
            flags          = VMA_ALLOCATION_CREATE_STRATEGY_BEST_FIT_BIT;
            mem_usage      = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
            device_address = true;
            break;
        }
        case EVkInternalBufferUsage::ShaderBuffer: {
            usage          = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
            flags          = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
            mem_usage      = VMA_MEMORY_USAGE_AUTO;
            device_address = true;
            break;
        }
        case EVkInternalBufferUsage::ShaderBuffer_Constant: {
            usage          = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
            flags          = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
            mem_usage      = VMA_MEMORY_USAGE_AUTO;
            device_address = true;
            break;
        }
    }
    VkBufferCreateInfo buffer_info = {
        .sType                 = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext                 = nullptr,
        .flags                 = 0,
        .size                  = _size,
        .usage                 = usage,
        .sharingMode           = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices   = nullptr
    };

#if WITH_CUDA
    buffer_info.pNext = GetExternalMemoryBufferCreateInfoPtr(nullptr);
#endif

    VmaAllocationCreateInfo   alloc_info{.flags = flags, .usage = mem_usage};
    VulkanBuffer::BufferAlloc buffer_alloc;
    BufferInfo                info(_size, 1, EBufferUsageFlags::NONE);

    VK_CHECK_RESULT(vmaCreateBuffer(
        _device.GetVmaAllocator(),
        &buffer_info,
        &alloc_info,
        &buffer_alloc.buffer,
        &buffer_alloc.alloc,
        nullptr
    ));

    static constexpr std::string_view usage_str[] = {
        "VkBackend::Upload",
        "VkBackend::Readback",
        "VkBackend::Scratch",
        "VkBackend::ShaderBuffer",
        "VkBackend::ShaderBuffer_Constant"
    };
    VulkanBuffer* vk_buffer = MoerNew(VulkanBuffer)(
        usage_str[uint(_usage)], info, _device, buffer_alloc.buffer, buffer_alloc.alloc, false, device_address
    );
    return vk_buffer;
}

MOER_TEMP_BUFFER_NOINLINE void DestroyTemporaryBuffer(VulkanBuffer* _buffer) {
    MoerDelete(_buffer);
}

#undef MOER_TEMP_BUFFER_NOINLINE

VulkanTemporaryBufferAllocator::LinearBufferAllocator::LinearBufferAllocator(
    VulkanDevice& _device,
    uint64        _initial_capacity,
    double        _growth_factor
) :
    device(_device),
    next_block_capacity(std::max<uint64>(_initial_capacity, 1)),
    growth_factor(_growth_factor) {
    allocated_buffers.push_back(
        {CreateTemporaryBuffer(device, next_block_capacity, MakeBufferName()), next_block_capacity, 0}
    );
}

VulkanTemporaryBufferAllocator::LinearBufferAllocator::BufferSlice
VulkanTemporaryBufferAllocator::LinearBufferAllocator::Allocate(uint64 _size, uint _alignment) {

    uint64 align_size = std::max(_alignment, 16u);
    for (auto& block : allocated_buffers) {
        auto offset = Moer::AlignUp(block.used_bytes, align_size);
        if (block.size_bytes - offset >= _size) {
            block.used_bytes = offset + _size;
            return {block.buffer, offset};
        }
    }

    align_size = std::max(align_size, _size);

    if (next_block_capacity < align_size) {
        next_block_capacity = std::max<uint64>(next_block_capacity * growth_factor, align_size);
    }

    auto buffer = CreateTemporaryBuffer(device, next_block_capacity, MakeBufferName());
    allocated_buffers.push_back({buffer, next_block_capacity, align_size});
    return {buffer, 0};
}

void VulkanTemporaryBufferAllocator::LinearBufferAllocator::Reset() {
    if (allocated_buffers.size() == 1) {
        allocated_buffers.back().used_bytes = 0;
    }
    if (allocated_buffers.size() > 1) {
        // Consolidate staging blocks for the next recording.
        uint64 total_size_bytes = 0;
        for (auto& block : allocated_buffers) {
            total_size_bytes += block.size_bytes;
            DestroyTemporaryBuffer(block.buffer);
        }
        allocated_buffers.clear();

        next_buffer_name_id = 0;

        allocated_buffers.push_back(
            {CreateTemporaryBuffer(device, total_size_bytes, MakeBufferName()), total_size_bytes, 0}
        );
    }
}

void VulkanTemporaryBufferAllocator::LinearBufferAllocator::Dispose() {
    for (auto& block : allocated_buffers) {
        DestroyTemporaryBuffer(block.buffer);
    }
    allocated_buffers.clear();
}

std::string VulkanTemporaryBufferAllocator::LinearBufferAllocator::MakeBufferName() {
    return std::format("VkBackend::LinearAllocBuffer_{}", next_buffer_name_id++);
}

VulkanTemporaryBufferAllocator::VulkanTemporaryBufferAllocator(VulkanDevice& _device) :
    device(_device),
    upload_allocator(_device, small_block_size, 1.5),
    readback_allocator(_device, small_block_size, 1.5) {}

VulkanTemporaryBufferAllocator::~VulkanTemporaryBufferAllocator() {
    upload_allocator.Dispose();
    readback_allocator.Dispose();
    ReleaseTemporaryBuffers();
}

BufferView VulkanTemporaryBufferAllocator::AllocateUploadBuffer(uint64 _size, uint _alignment) {
    _size = std::max<uint64>(_size, _alignment);
    if (_size < small_block_size) {
        const LinearBufferAllocator::BufferSlice slice = upload_allocator.Allocate(_size, _alignment);
        return {slice.buffer, slice.byte_offset, _size, 1u};
    }
    VulkanBuffer* buffer =
        CreateTemporaryBuffer(device, _size, std::format("VkBackend::LargeUploadBuffer_{}", _size));
    temporary_buffers.push_back(buffer);
    return {buffer, 0, _size, 1u};
}

BufferView VulkanTemporaryBufferAllocator::AllocateReadbackBuffer(uint64 _size, uint _alignment) {
    _size = std::max<uint64>(_size, _alignment);
    if (_size < small_block_size) {
        const LinearBufferAllocator::BufferSlice slice = readback_allocator.Allocate(_size, _alignment);
        return {slice.buffer, slice.byte_offset, _size, 1u};
    }
    VulkanBuffer* buffer =
        CreateTemporaryBuffer(device, _size, std::format("VkBackend::LargeReadbackBuffer_{}", _size));
    temporary_buffers.push_back(buffer);

    return {buffer, 0, _size, 1u};
}

BufferView VulkanTemporaryBufferAllocator::AllocateScratch(uint64 _size) {
    VulkanBuffer* buffer = CreateTemporaryBuffer(device, _size, EVkInternalBufferUsage::Scratch);
    temporary_buffers.push_back(buffer);
    return {buffer, 0, buffer->GetNumElement(), 1u};
}

BufferView VulkanTemporaryBufferAllocator::AllocateShaderBuffer(uint64 _size) {
    VulkanBuffer* buffer = CreateTemporaryBuffer(device, _size, EVkInternalBufferUsage::ShaderBuffer);
    temporary_buffers.push_back(buffer);
    return {buffer, 0, buffer->GetNumElement(), 1u};
}

void VulkanTemporaryBufferAllocator::Reset() {
    upload_allocator.Reset();
    readback_allocator.Reset();
    ReleaseTemporaryBuffers();
}

void VulkanTemporaryBufferAllocator::ReleaseTemporaryBuffers() {
    for (VulkanBuffer* buffer : temporary_buffers) {
        DestroyTemporaryBuffer(buffer);
    }
    temporary_buffers.clear();
}

} // namespace Moer::Render
