#include "VulkanTemporaryBufferAllocator.h"
#include "VulkanRHIResource.h"
#include "misc/Alignment.h"
#include "misc/MMemory.h"

#include <algorithm>
#include <format>
#include <stdexcept>
#include <string_view>

namespace Moer::Render {

namespace {

VulkanBufferAllocationInfo BuildTemporaryBufferAllocationInfo(EVkInternalBufferUsage _usage) {
    VkBufferUsageFlags       usage{};
    VmaAllocationCreateFlags flags{};
    VmaMemoryUsage           mem_usage      = VMA_MEMORY_USAGE_AUTO;
    bool                     device_address = false;
    switch (_usage) {

        case EVkInternalBufferUsage::Upload: {
            usage     = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            flags     = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
            mem_usage = VMA_MEMORY_USAGE_AUTO;
            break;
        }
        case EVkInternalBufferUsage::Readback: {
            usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
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
        default:
            throw std::invalid_argument("unsupported temporary Vulkan buffer usage");
    }
    return VulkanBufferAllocationInfo{
        .vk_usage             = usage,
        .allocation_flags     = flags,
        .memory_usage         = mem_usage,
        .needs_device_address = device_address,
    };
}

} // namespace

// Keep these functions addressable and out of line for the optional profiler's
// PDB hooks. Their implementation stays local to this translation unit.
#if defined(_MSC_VER)
#define MOER_TEMP_BUFFER_NOINLINE __declspec(noinline)
#else
#define MOER_TEMP_BUFFER_NOINLINE __attribute__((noinline))
#endif

MOER_TEMP_BUFFER_NOINLINE VulkanBuffer* CreateTemporaryBuffer(
    VulkanDevice&          _device,
    uint64                 _size,
    std::string_view       _name,
    EVkInternalBufferUsage _usage
) {
    const EBufferUsageFlags info_usage =
        _usage == EVkInternalBufferUsage::Upload || _usage == EVkInternalBufferUsage::Readback ?
            EBufferUsageFlags::CPU_VISIBLE :
            EBufferUsageFlags::NONE;
    const BufferInfo info(_size, 1, info_usage);
    return MoerNew(VulkanBuffer)(_name, info, _device, BuildTemporaryBufferAllocationInfo(_usage), false);
}

MOER_TEMP_BUFFER_NOINLINE void DestroyTemporaryBuffer(VulkanBuffer* _buffer) {
    MoerDelete(_buffer);
}

#undef MOER_TEMP_BUFFER_NOINLINE

VulkanTemporaryBufferAllocator::LinearBufferAllocator::LinearBufferAllocator(
    VulkanDevice&          _device,
    EVkInternalBufferUsage _buffer_usage,
    uint64                 _initial_capacity,
    double                 _growth_factor
) :
    device(_device),
    buffer_usage(_buffer_usage),
    next_block_capacity(std::clamp<uint64>(_initial_capacity, 1, max_block_size)),
    growth_factor(_growth_factor) {
    allocated_buffers.push_back({CreateBufferBlock(next_block_capacity), next_block_capacity, 0});
}

VulkanTemporaryBufferAllocator::LinearBufferAllocator::BufferSlice
VulkanTemporaryBufferAllocator::LinearBufferAllocator::Allocate(uint64 _size, uint _alignment) {

    const uint64 align_size        = std::max(_alignment, 16u);
    const uint64 required_capacity = std::max(align_size, _size);
    if (required_capacity > max_block_size) {
        throw std::invalid_argument("linear Vulkan buffer allocation exceeds the maximum block size");
    }
    for (auto& block : allocated_buffers) {
        auto offset = Moer::AlignUp(block.used_bytes, align_size);
        if (offset <= block.size_bytes && block.size_bytes - offset >= _size) {
            block.used_bytes = offset + _size;
            return {block.buffer, offset};
        }
    }

    const uint64 grown_capacity =
        static_cast<uint64>(std::min<double>(next_block_capacity * growth_factor, max_block_size));
    const uint64 block_capacity = std::max(grown_capacity, required_capacity);
    auto         buffer         = CreateBufferBlock(block_capacity);
    allocated_buffers.push_back({buffer, block_capacity, required_capacity});
    next_block_capacity = block_capacity;
    return {buffer, 0};
}

void VulkanTemporaryBufferAllocator::LinearBufferAllocator::Reset() {
    if (allocated_buffers.size() == 1) {
        allocated_buffers.back().used_bytes = 0;
    }
    if (allocated_buffers.size() > 1) {
        // Keep a bounded staging cache for the next recording.
        uint64 total_size_bytes = 0;
        for (auto& block : allocated_buffers) {
            total_size_bytes = std::min(total_size_bytes + block.size_bytes, max_block_size);
            DestroyTemporaryBuffer(block.buffer);
        }
        allocated_buffers.clear();

        next_buffer_name_id = 0;

        allocated_buffers.push_back({CreateBufferBlock(total_size_bytes), total_size_bytes, 0});
    }
    next_block_capacity = allocated_buffers.empty() ?
                              small_block_size :
                              std::min(allocated_buffers.front().size_bytes, max_block_size);
}

void VulkanTemporaryBufferAllocator::LinearBufferAllocator::Dispose() {
    for (auto& block : allocated_buffers) {
        DestroyTemporaryBuffer(block.buffer);
    }
    allocated_buffers.clear();
}

VulkanBuffer* VulkanTemporaryBufferAllocator::LinearBufferAllocator::CreateBufferBlock(uint64 _size) {
    return CreateTemporaryBuffer(device, _size, MakeBufferName(), buffer_usage);
}

std::string VulkanTemporaryBufferAllocator::LinearBufferAllocator::MakeBufferName() {
    return std::format("VkBackend::LinearAllocBuffer_{}", next_buffer_name_id++);
}

VulkanTemporaryBufferAllocator::VulkanTemporaryBufferAllocator(VulkanDevice& _device) :
    device(_device),
    upload_allocator(_device, EVkInternalBufferUsage::Upload, small_block_size, 1.5),
    readback_allocator(_device, EVkInternalBufferUsage::Readback, small_block_size, 1.5) {}

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
    VulkanBuffer* buffer = CreateTemporaryBuffer(
        device, _size, std::format("VkBackend::LargeUploadBuffer_{}", _size), EVkInternalBufferUsage::Upload
    );
    temporary_buffers.push_back(buffer);
    return {buffer, 0, _size, 1u};
}

BufferView VulkanTemporaryBufferAllocator::AllocateReadbackBuffer(uint64 _size, uint _alignment) {
    _size = std::max<uint64>(_size, _alignment);
    if (_size < small_block_size) {
        const LinearBufferAllocator::BufferSlice slice = readback_allocator.Allocate(_size, _alignment);
        return {slice.buffer, slice.byte_offset, _size, 1u};
    }
    VulkanBuffer* buffer = CreateTemporaryBuffer(
        device,
        _size,
        std::format("VkBackend::LargeReadbackBuffer_{}", _size),
        EVkInternalBufferUsage::Readback
    );
    temporary_buffers.push_back(buffer);

    return {buffer, 0, _size, 1u};
}

BufferView VulkanTemporaryBufferAllocator::AllocateScratch(uint64 _size) {
    VulkanBuffer* buffer =
        CreateTemporaryBuffer(device, _size, "VkBackend::Scratch", EVkInternalBufferUsage::Scratch);
    temporary_buffers.push_back(buffer);
    return {buffer, 0, buffer->GetNumElement(), 1u};
}

BufferView VulkanTemporaryBufferAllocator::AllocateShaderBuffer(uint64 _size) {
    VulkanBuffer* buffer =
        CreateTemporaryBuffer(device, _size, "VkBackend::ShaderBuffer", EVkInternalBufferUsage::ShaderBuffer);
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
