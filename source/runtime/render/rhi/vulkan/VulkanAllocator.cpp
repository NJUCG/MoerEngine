#include "VulkanAllocator.h"
#include "PixelFormat.h"
#include "VulkanDevice.h"
#include "VulkanMacroUtils.h"
#include "VulkanRHIResource.h"
#include "VulkanResourceTracker.h"
#include "vulkan/vulkan_core.h"
namespace Moer::Render {

namespace {

void UnlockGraphEventOnTaskGraph(const GraphEventRef& event) {
    if (!event) {
        return;
    }
    GraphTask<EmptyGraphTask>::Create(EThread::AnyThread_NormalPri)
        .Next(event)
        .Dispatch(EThread::AnyThread_NormalPri);
}

void ExecuteCompletionEvents(Array<GraphEventRef>& completion_events) {
    for (const auto& event : completion_events) {
        UnlockGraphEventOnTaskGraph(event);
    }
    completion_events.clear();
}

} // namespace

// VulkanAllocatorBase
VulkanAllocatorBase::VulkanAllocatorBase(VulkanDevice* _device, EQueueType _type) :
    VulkanDeviceObject(_device),
    tracker(_type) {
    cmd_allocator.emplace(_device, _type);
    cmd_list.emplace(&cmd_allocator.value(), *_device);
}

VulkanAllocatorBase::~VulkanAllocatorBase() {
    cmd_list.reset();
    cmd_allocator.reset();
    on_complete.clear();
    completion_events.clear();
    tracker.Reset();
}

void VulkanAllocatorBase::ResetCmdList() {
    vkResetCommandPool(m_device->GetDevice(), cmd_allocator->GetHandle(), 0);
}

void VulkanAllocatorBase::Complete(VulkanFence* _fence, uint64 _wait_val) {
    _fence->Notify(_wait_val);
    //execute post complete functions if needed
    for (auto& func : on_complete) {
        func();
    }
    on_complete.clear();
    ExecuteCompletionEvents(completion_events);
}

void VulkanAllocatorBase::Reset() {
    ResetCmdList();
}

// VulkanPresentor
VulkanPresentor::VulkanPresentor(VulkanDevice* _device, EQueueType _type) :
    VulkanAllocatorBase(_device, _type) {}

VulkanPresentor::~VulkanPresentor() {}

void VulkanPresentor::Complete(VulkanFence* _fence, uint64 _wait_val) {
    _fence->HostWait(_wait_val);
    _fence->Notify(_wait_val);
    //execute post complete functions if needed
    for (auto& func : on_complete) {
        func();
    }
    on_complete.clear();
    ExecuteCompletionEvents(completion_events);
}

#pragma region[ Query Pool ]

VkNativeQueryPool::VkNativeQueryPool(VulkanDevice& _device, VkQueryType _type, uint32 _query_count) :
    device(_device),
    type(_type),
    count(_query_count) {
    VkQueryPoolCreateInfo create_info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    create_info.queryType  = _type;
    create_info.queryCount = _query_count;
    vkCreateQueryPool(device.GetDevice(), &create_info, nullptr, &query_pool);
}

VkNativeQueryPool::~VkNativeQueryPool() {
    vkDestroyQueryPool(device.GetDevice(), query_pool, nullptr);
}

void VkNativeQueryPool::GetResults(
    std::span<uint64>  _data,
    uint32             _first_query,
    uint32             _query_count,
    VkQueryResultFlags _flags
) {
    vkGetQueryPoolResults(
        device.GetDevice(),
        query_pool,
        _first_query,
        _query_count,
        _data.size_bytes(),
        _data.data(),
        sizeof(uint64),
        VK_QUERY_RESULT_64_BIT | _flags
    );
}

#pragma endregion

// VulkanAllocator
VulkanAllocator::VulkanAllocator(VulkanDevice* _device, EQueueType _type) :
    VulkanAllocatorBase(_device, _type),
    allocator(_device),
    upload_allocator(&allocator, small_block_size, 1.5),
    readback_allocator(&allocator, small_block_size, 1.5),
    scratch_allocator(_device, &allocator),
    shader_buffer_allocator(_device, &allocator),
    timestamp_pool(*_device, VK_QUERY_TYPE_TIMESTAMP, 1000) {}

VulkanAllocator::~VulkanAllocator() {
    upload_allocator.Dispose();
    readback_allocator.Dispose();
    scratch_allocator.Reset();
    shader_buffer_allocator.Reset();
    for (auto& handle : large_buffers) {
        allocator.DeAllocate(reinterpret_cast<uint64>(handle));
    }
    large_buffers.clear();
}

BufferView VulkanAllocator::AllocateUploadBuffer(uint64 _size, uint _alignment) {
    _size = std::max<uint64>(_size, _alignment);
    if (_size < small_block_size) {
        auto          handle = upload_allocator.Allocate(_size, _alignment);
        VulkanBuffer* buffer = reinterpret_cast<VulkanBuffer*>(handle.handle);
        return {buffer, handle.offset, _size, 1u};
    }
    auto          handle = allocator.Allocate(_size, std::format("VkBackend::LargeUploadBuffer_{}", _size));
    VulkanBuffer* buffer = reinterpret_cast<VulkanBuffer*>(handle);
    large_buffers.push_back(buffer);
    return {buffer, 0, _size, 1u};
}

BufferView VulkanAllocator::AllocateReadbackBuffer(uint64 _size, uint _alignment) {
    _size = std::max<uint64>(_size, _alignment);
    if (_size < small_block_size) {
        auto          handle = readback_allocator.Allocate(_size, _alignment);
        VulkanBuffer* buffer = reinterpret_cast<VulkanBuffer*>(handle.handle);
        return {buffer, handle.offset, _size, 1u};
    }
    auto          handle = allocator.Allocate(_size, std::format("VkBackend::LargeReadbackBuffer_{}", _size));
    VulkanBuffer* buffer = reinterpret_cast<VulkanBuffer*>(handle);
    large_buffers.push_back(buffer);

    return {buffer, 0, _size, 1u};
}

void VulkanAllocator::ResetBufferAlloc() {
    upload_allocator.Reset();
    readback_allocator.Reset();
    for (auto& handle : large_buffers) {
        allocator.DeAllocate(reinterpret_cast<uint64>(handle));
    }
    large_buffers.clear();
    scratch_allocator.Reset();
    shader_buffer_allocator.Reset();
}

void VulkanAllocator::Complete(VulkanFence* _fence, uint64 _wait_val) {
    _fence->HostWait(_wait_val);
    _fence->Notify(std::max(_wait_val, _fence->current_value));
    //execute post complete functions if needed
    for (auto& func : on_complete) {
        func();
    }
    on_complete.clear();
    ExecuteCompletionEvents(completion_events);
}

void VulkanAllocator::Reset() {
    ResetBufferAlloc();
    ResetCmdList();
}

VkTmpBufferAllocator::VkTmpBufferAllocator(VulkanDevice* _device) : VulkanDeviceObject(_device) {}
uint64 VkTmpBufferAllocator::Allocate(uint64 _size, std::string_view _name) {
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
        m_device->GetVmaAllocator(),
        &buffer_info,
        &alloc_info,
        &buffer_alloc.buffer,
        &buffer_alloc.alloc,
        nullptr
    ));
    VulkanBuffer* vk_buffer =
        MoerNew(VulkanBuffer)(_name, info, *m_device, buffer_alloc.buffer, buffer_alloc.alloc, false);

    return reinterpret_cast<uint64>(vk_buffer);
}

uint64 VkTmpBufferAllocator::Allocate(uint64 _size, EVkInternalBufferUsage _usage) {

    VkBufferUsageFlags       usage{};
    VmaAllocationCreateFlags flags{};
    VmaMemoryUsage           mem_usage     = VMA_MEMORY_USAGE_AUTO;
    bool                     devce_address = false;
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
            flags         = VMA_ALLOCATION_CREATE_STRATEGY_BEST_FIT_BIT;
            mem_usage     = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
            devce_address = true;
            break;
        }
        case EVkInternalBufferUsage::ShaderBuffer: {
            usage         = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
            flags         = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
            mem_usage     = VMA_MEMORY_USAGE_AUTO;
            devce_address = true;
            break;
        }
        case EVkInternalBufferUsage::ShaderBuffer_Constant: {
            usage         = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
            flags         = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
            mem_usage     = VMA_MEMORY_USAGE_AUTO;
            devce_address = true;
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
        m_device->GetVmaAllocator(),
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
        usage_str[uint(_usage)],
        info,
        *m_device,
        buffer_alloc.buffer,
        buffer_alloc.alloc,
        false,
        devce_address
    );
    return reinterpret_cast<uint64>(vk_buffer);
}

void VkTmpBufferAllocator::DeAllocate(uint64 _buffer) {
    auto* buffer = reinterpret_cast<VulkanBuffer*>(_buffer);
    MoerDelete(buffer);
}

VulkanAllocator::ScratchAllocator::ScratchAllocator(VulkanDevice* _device, VkTmpBufferAllocator* _allocator) :
    VulkanDeviceObject(_device),
    allocator(_allocator) {
    alignment = _device->GetOptionalProperties()
                    .acceleration_structure_properties.minAccelerationStructureScratchOffsetAlignment;
}

uint64 VulkanAllocator::ScratchAllocator::Allocate(uint64 _size) {
    uint64 handle = allocator->Allocate(_size, EVkInternalBufferUsage::Scratch);
    allocated_buffers.push_back(handle);
    return handle;
}

BufferView VulkanAllocator::AllocateScratch(uint64 _size) {
    auto          handle = scratch_allocator.Allocate(_size);
    VulkanBuffer* buffer = reinterpret_cast<VulkanBuffer*>(handle);
    return {buffer, 0, buffer->GetNumElement(), 1u};
}

void VulkanAllocator::ScratchAllocator::Deallocate(uint64 _handle) {
    allocator->DeAllocate(_handle);
}

void VulkanAllocator::ScratchAllocator::Reset() {
    for (uint64 handle : allocated_buffers) {
        Deallocate(handle);
    }
    allocated_buffers.clear();
}

VulkanAllocator::StackAllocator::StackAllocator(
    VkTmpBufferAllocator* _alloc,
    uint64                _init_cap,
    double                _grouth_factor
) :
    allocator(_alloc),
    init_capacity(_init_cap),
    growth_factor(_grouth_factor),
    stack_memory_id(0) {
    capacity = std::max<uint64>(init_capacity, 1);
    allocated_buffers.push_back({allocator->Allocate(capacity, GetStackBufferName()), capacity, 0});
}

VulkanAllocator::StackAllocator::Chunk VulkanAllocator::StackAllocator::Allocate(uint64 _size, uint _align) {

    uint64 align_size = std::max(_align, 16u);
    for (auto& alloc_buf : allocated_buffers) {
        auto offset = Moer::AlignUp(alloc_buf.offset, align_size);
        if (alloc_buf.size - offset >= _size) {
            alloc_buf.offset = offset + _size;
            return {alloc_buf.handle, offset};
        }
    }

    align_size = std::max(align_size, _size);

    if (capacity < align_size) {
        capacity = std::max<uint64>(capacity * growth_factor, align_size);
    }

    auto buffer = allocator->Allocate(capacity, GetStackBufferName());
    allocated_buffers.push_back({buffer, capacity, align_size});
    return {buffer, 0};
}

VulkanAllocator::StackAllocator::Chunk VulkanAllocator::StackAllocator::Allocate(uint64 _size) {
    for (auto& alloc_buf : allocated_buffers) {
        if (alloc_buf.size - alloc_buf.offset >= _size) {
            auto offset = alloc_buf.offset;
            alloc_buf.offset += _size;
            return {alloc_buf.handle, offset};
        }
    }

    if (capacity < _size) {
        capacity = std::max<uint64>(capacity * growth_factor, _size);
    }

    auto buffer = allocator->Allocate(capacity, GetStackBufferName());
    allocated_buffers.push_back({buffer, capacity, _size});
    return {buffer, 0};
}

void VulkanAllocator::StackAllocator::Reset() {
    if (allocated_buffers.size() == 1) {
        allocated_buffers.back().offset = 0;
    }
    if (allocated_buffers.size() > 1) {
        //pack all staging buffer to one
        uint64 sum_size = 0;
        for (auto& alloc_buf : allocated_buffers) {
            sum_size += alloc_buf.size;
            allocator->DeAllocate(alloc_buf.handle);
        }
        allocated_buffers.clear();

        stack_memory_id = 0;

        allocated_buffers.push_back({allocator->Allocate(sum_size, GetStackBufferName()), sum_size, 0});
    }
}

void VulkanAllocator::StackAllocator::Dispose() {
    for (auto& alloc_buf : allocated_buffers) {
        allocator->DeAllocate(alloc_buf.handle);
    }
    allocated_buffers.clear();
}

std::string VulkanAllocator::StackAllocator::GetStackBufferName() {
    return std::format("VkBackend::StackAllocBuffer_{}", stack_memory_id++);
}

VulkanAllocator::ShaderBufferAllocator::ShaderBufferAllocator(
    VulkanDevice*         _device,
    VkTmpBufferAllocator* _allocator
) :
    VulkanDeviceObject(_device),
    allocator(_allocator) {}

uint64 VulkanAllocator::ShaderBufferAllocator::Allocate(uint64 _size) {
    uint64 handle = allocator->Allocate(_size, EVkInternalBufferUsage::ShaderBuffer);
    allocated_buffers.push_back(handle);
    return handle;
}

void VulkanAllocator::ShaderBufferAllocator::Deallocate(uint64 _handle) {
    allocator->DeAllocate(_handle);
}

void VulkanAllocator::ShaderBufferAllocator::Reset() {
    for (uint64 handle : allocated_buffers) {
        Deallocate(handle);
    }
    allocated_buffers.clear();
}

BufferView VulkanAllocator::AllocateShaderBuffer(uint64 _size) {
    auto          handle = shader_buffer_allocator.Allocate(_size);
    VulkanBuffer* buffer = reinterpret_cast<VulkanBuffer*>(handle);
    return {buffer, 0, buffer->GetNumElement(), 1u};
}

VulkanCmdAllocator::VulkanCmdAllocator(VulkanDevice* _device, EQueueType _queue_type) :
    VulkanDeviceObject(_device),
    queue_type(VulkanEnumTranslator::METoVKQueueFlagBits(_queue_type)) {
    // 必须使用 GetQueueFamilyIndex(EQueueType) 而非 GetQueueFamilyIndex(VkQueueFlags)。
    // 后者根据硬件能力搜索，可能返回不同的 family（如 AMD 上返回 compute family），
    // 而前者使用引擎已确定的、可能经过 override 的 family index。
    uint32_t family_index = m_device->GetQueueFamilyIndex(_queue_type);

    VkCommandPoolCreateInfo pool_info = {
        .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext            = nullptr,
        .flags            = 0,
        .queueFamilyIndex = family_index
    };

    VK_CHECK_RESULT(vkCreateCommandPool(_device->GetDevice(), &pool_info, nullptr, &command_pool));
}

VulkanCmdAllocator::~VulkanCmdAllocator() {
    if (command_pool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(m_device->GetDevice(), command_pool, nullptr);
        command_pool = VK_NULL_HANDLE;
    }
}
} // namespace Moer::Render
