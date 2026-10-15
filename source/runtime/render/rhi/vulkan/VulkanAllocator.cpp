#include "VulkanAllocator.h"
#include "VulkanDevice.h"
#include "VulkanMacroUtils.h"
#include "VulkanRHIResource.h"

#include <algorithm>
#include <stdexcept>

namespace Moer::Render {

namespace {

bool InvokeAllocatorCompletionCallbacksNoexcept(
    Array<std::function<void()>>& _callbacks,
    std::string_view              _owner
) noexcept {
    bool succeeded = true;
    for (auto& callback : _callbacks) {
        try {
            if (callback) {
                callback();
            }
        } catch (const std::exception& error) {
            succeeded = false;
            try {
                LOG_ERROR("[RHIExecutor][Vulkan] {} completion callback threw: {}", _owner, error.what());
            } catch (...) {
            }
        } catch (...) {
            succeeded = false;
            try {
                LOG_ERROR("[RHIExecutor][Vulkan] {} completion callback threw", _owner);
            } catch (...) {
            }
        }
    }
    _callbacks.clear();
    return succeeded;
}

} // namespace

VulkanCommandPool::VulkanCommandPool(VulkanDevice* _device, EQueueType _queue_type) :
    VulkanDeviceObject(_device),
    queue_type(_queue_type) {
    // Use the selected queue-role mapping, including vendor overrides, so the
    // command pool belongs to the same family as the actual submission queue.
    uint32_t family_index = m_device->GetQueueFamilyIndex(_queue_type);

    VkCommandPoolCreateInfo pool_info = {
        .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext            = nullptr,
        .flags            = 0,
        .queueFamilyIndex = family_index
    };

    VK_CHECK_RESULT(vkCreateCommandPool(_device->GetDevice(), &pool_info, nullptr, &command_pool));
}

VulkanCommandPool::~VulkanCommandPool() {
    if (command_pool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(m_device->GetDevice(), command_pool, nullptr);
        command_pool = VK_NULL_HANDLE;
    }
}

std::string_view VulkanCommandPool::GetQueueName() const {
    switch (queue_type) {
        case EQueueType::Graphics:
            return "Graphics";
        case EQueueType::Compute:
            return "Compute";
        case EQueueType::Copy:
            return "Copy";
        default:
            return "Unknown";
    }
}

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

// VulkanAllocatorBase
VulkanAllocatorBase::VulkanAllocatorBase(VulkanDevice* _device, EQueueType _type) :
    VulkanDeviceObject(_device),
    queue_type(_type),
    tracker(_type) {
    command_pool.emplace(_device, _type);
    cmd_list.emplace(&command_pool.value(), *_device);
}

VulkanAllocatorBase::~VulkanAllocatorBase() {
    secondary_cmd_list.reset();
    cmd_list.reset();
    command_pool.reset();
    on_complete.clear();
    tracker.Reset();
}

VulkanCmdList& VulkanAllocatorBase::PrepareSecondaryCmdList() {
    if (!secondary_cmd_list.has_value()) {
        secondary_cmd_list.emplace(&command_pool.value(), *m_device, VK_COMMAND_BUFFER_LEVEL_SECONDARY);
    }
    return secondary_cmd_list.value();
}

bool VulkanAllocatorBase::ResetCmdList() {
    if (m_device->IsDeviceLost()) {
        m_device->RecordSkippedCommandPoolReset();
        return false;
    }

    VkQueue queue = VK_NULL_HANDLE;
    switch (queue_type) {
        case EQueueType::Graphics:
            queue = m_device->GetGraphicsQueue();
            break;
        case EQueueType::Compute:
            queue = m_device->GetComputeQueue();
            break;
        case EQueueType::Copy:
            queue = m_device->GetTransferQueue();
            break;
        default:
            break;
    }

    const VkResult result = m_device->ResetCommandPool(
        command_pool->GetHandle(),
        VulkanOperationContext{
            .operation  = EVulkanFaultOperation::CommandPoolReset,
            .queue_type = queue_type,
            .queue      = queue,
        }
    );
    if (result == VK_SUCCESS) {
        cmd_list->SetDescriptorPushLease({});
        if (secondary_cmd_list.has_value()) {
            secondary_cmd_list->SetDescriptorPushLease({});
        }
        return true;
    }
    return false;
}

bool VulkanAllocatorBase::CompleteSuccess() noexcept {
    return InvokeAllocatorCompletionCallbacksNoexcept(on_complete, "allocator");
}

bool VulkanAllocatorBase::Reset() {
    return ResetCmdList();
}

bool VulkanAllocatorBase::ResetAbandoned() {
    on_complete.clear();
    tracker.Reset();
    return Reset();
}

// VulkanPresentor
VulkanPresentor::VulkanPresentor(VulkanDevice* _device, EQueueType _type) :
    VulkanAllocatorBase(_device, _type) {
    VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    const VkResult        result =
        vkCreateSemaphore(m_device->GetDevice(), &semaphore_info, VK_NULL_HANDLE, &image_ready_semaphore);
    if (result != VK_SUCCESS) {
        image_ready_semaphore = VK_NULL_HANDLE;
        m_device->TryLatchFirstFault(
            VulkanOperationContext{
                .operation  = EVulkanFaultOperation::SwapchainSemaphoreCreate,
                .queue_type = _type,
            },
            result
        );
        throw std::runtime_error("failed to create Vulkan presentor image-ready semaphore");
    }
    try {
        m_device->SetResourceName(
            uint64(image_ready_semaphore),
            VK_OBJECT_TYPE_SEMAPHORE,
            "PresentorImageReadySemaphore_" + std::to_string(reinterpret_cast<uintptr_t>(this))
        );
    } catch (...) {
        vkDestroySemaphore(m_device->GetDevice(), image_ready_semaphore, VK_NULL_HANDLE);
        image_ready_semaphore = VK_NULL_HANDLE;
        throw;
    }
}

VulkanPresentor::~VulkanPresentor() {
    if (image_ready_semaphore != VK_NULL_HANDLE) {
        vkDestroySemaphore(m_device->GetDevice(), image_ready_semaphore, VK_NULL_HANDLE);
        image_ready_semaphore = VK_NULL_HANDLE;
    }
}

bool VulkanPresentor::CompleteSuccess() noexcept {
    return InvokeAllocatorCompletionCallbacksNoexcept(on_complete, "presentor");
}

VulkanAllocator::LinearBufferAllocator::LinearBufferAllocator(
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

VulkanAllocator::LinearBufferAllocator::BufferSlice
VulkanAllocator::LinearBufferAllocator::Allocate(uint64 _size, uint _alignment) {

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

void VulkanAllocator::LinearBufferAllocator::Reset() {
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

void VulkanAllocator::LinearBufferAllocator::Dispose() {
    for (auto& block : allocated_buffers) {
        DestroyTemporaryBuffer(block.buffer);
    }
    allocated_buffers.clear();
}

std::string VulkanAllocator::LinearBufferAllocator::MakeBufferName() {
    return std::format("VkBackend::LinearAllocBuffer_{}", next_buffer_name_id++);
}

// VulkanAllocator
VulkanAllocator::VulkanAllocator(VulkanDevice* _device, EQueueType _type) :
    VulkanAllocatorBase(_device, _type),
    upload_allocator(*_device, small_block_size, 1.5),
    readback_allocator(*_device, small_block_size, 1.5),
    queries(*_device, _type) {}

VulkanAllocator::~VulkanAllocator() {
    upload_allocator.Dispose();
    readback_allocator.Dispose();
    ReleaseTemporaryBuffers();
}

BufferView VulkanAllocator::AllocateUploadBuffer(uint64 _size, uint _alignment) {
    _size = std::max<uint64>(_size, _alignment);
    if (_size < small_block_size) {
        const LinearBufferAllocator::BufferSlice slice = upload_allocator.Allocate(_size, _alignment);
        return {slice.buffer, slice.byte_offset, _size, 1u};
    }
    VulkanBuffer* buffer =
        CreateTemporaryBuffer(*m_device, _size, std::format("VkBackend::LargeUploadBuffer_{}", _size));
    temporary_buffers.push_back(buffer);
    return {buffer, 0, _size, 1u};
}

BufferView VulkanAllocator::AllocateReadbackBuffer(uint64 _size, uint _alignment) {
    _size = std::max<uint64>(_size, _alignment);
    if (_size < small_block_size) {
        const LinearBufferAllocator::BufferSlice slice = readback_allocator.Allocate(_size, _alignment);
        return {slice.buffer, slice.byte_offset, _size, 1u};
    }
    VulkanBuffer* buffer =
        CreateTemporaryBuffer(*m_device, _size, std::format("VkBackend::LargeReadbackBuffer_{}", _size));
    temporary_buffers.push_back(buffer);

    return {buffer, 0, _size, 1u};
}

BufferView VulkanAllocator::AllocateScratch(uint64 _size) {
    VulkanBuffer* buffer = CreateTemporaryBuffer(*m_device, _size, EVkInternalBufferUsage::Scratch);
    temporary_buffers.push_back(buffer);
    return {buffer, 0, buffer->GetNumElement(), 1u};
}

BufferView VulkanAllocator::AllocateShaderBuffer(uint64 _size) {
    VulkanBuffer* buffer = CreateTemporaryBuffer(*m_device, _size, EVkInternalBufferUsage::ShaderBuffer);
    temporary_buffers.push_back(buffer);
    return {buffer, 0, buffer->GetNumElement(), 1u};
}

void VulkanAllocator::ResetBufferAlloc() {
    upload_allocator.Reset();
    readback_allocator.Reset();
    ReleaseTemporaryBuffers();
}

void VulkanAllocator::ReleaseTemporaryBuffers() {
    for (VulkanBuffer* buffer : temporary_buffers) {
        DestroyTemporaryBuffer(buffer);
    }
    temporary_buffers.clear();
}

void VulkanAllocator::EnsureQueryCapacity(size_t _timestamp_slot_count, size_t _occlusion_pair_count) {
    queries.EnsureQueryCapacity(_timestamp_slot_count, _occlusion_pair_count);
}

void VulkanAllocator::ConfigureTimestampCapabilityForRecording(
    uint32                         _valid_bits,
    EVulkanTimestampQueryResetMode _reset_mode
) {
    queries.ConfigureTimestampCapabilityForRecording(_valid_bits, _reset_mode);
}

void VulkanAllocator::RecordQuery(VulkanCmdList& _cmd_list, const QueryCmd& _cmd) {
    queries.RecordQuery(_cmd_list, _cmd);
}

VkResult VulkanAllocator::PrepareQueriesAfterGpuCompletion(const VulkanOperationContext& _context) noexcept {
    return queries.PrepareQueriesAfterGpuCompletion(_context);
}

bool VulkanAllocator::CompleteSuccessCallbacks() noexcept {
    return InvokeAllocatorCompletionCallbacksNoexcept(on_complete, "allocator");
}

void VulkanAllocator::PublishQueriesAfterGpuCompletion(
    QueryPublishBatch _batch,
    bool              _gpu_success,
    std::string_view  _failure_reason
) noexcept {
    queries.PublishQueriesAfterGpuCompletion(_batch, _gpu_success, _failure_reason);
}

void VulkanAllocator::NotifyQueriesAfterGpuCompletion(QueryPublishBatch _batch) noexcept {
    queries.NotifyQueriesAfterGpuCompletion(_batch);
}

bool VulkanAllocator::CompleteSuccess() noexcept {
    VulkanOperationContext context{
        .operation  = EVulkanFaultOperation::QueryPoolResults,
        .queue_type = queue_type,
    };
    const VkResult          prepare_result      = PrepareQueriesAfterGpuCompletion(context);
    const bool              callbacks_succeeded = prepare_result == VK_SUCCESS && CompleteSuccessCallbacks();
    const QueryPublishBatch query_batch         = QueryBackendAccess::BeginPublishBatch();
    PublishQueriesAfterGpuCompletion(
        query_batch, prepare_result == VK_SUCCESS, "Vulkan query result preparation failed"
    );
    NotifyQueriesAfterGpuCompletion(query_batch);
    return prepare_result == VK_SUCCESS && callbacks_succeeded;
}

bool VulkanAllocator::Reset() {
    if (!ResetCmdList()) {
        return false;
    }
    ResetBufferAlloc();
    queries.ClearRecords();
    return true;
}

} // namespace Moer::Render
