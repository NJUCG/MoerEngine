#include "VulkanCommandContext.h"
#include "VulkanDevice.h"

#include <stdexcept>
#include <string>

namespace Moer::Render {

namespace {

bool InvokeCompletionCallbacksNoexcept(
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

// VulkanCommandContextBase
VulkanCommandContextBase::VulkanCommandContextBase(VulkanDevice* _device, EQueueType _type) :
    VulkanDeviceObject(_device),
    queue_type(_type),
    tracker(_type) {
    command_pool.emplace(_device, _type);
    cmd_list.emplace(&command_pool.value(), *_device);
}

VulkanCommandContextBase::~VulkanCommandContextBase() {
    secondary_cmd_list.reset();
    cmd_list.reset();
    command_pool.reset();
    on_complete.clear();
    tracker.Reset();
}

VulkanCmdList& VulkanCommandContextBase::PrepareSecondaryCmdList() {
    if (!secondary_cmd_list.has_value()) {
        secondary_cmd_list.emplace(&command_pool.value(), *m_device, VK_COMMAND_BUFFER_LEVEL_SECONDARY);
    }
    return secondary_cmd_list.value();
}

bool VulkanCommandContextBase::ResetCommandPool() {
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

bool VulkanCommandContextBase::CompleteSuccess() noexcept {
    return InvokeCompletionCallbacksNoexcept(on_complete, "command context");
}

bool VulkanCommandContextBase::Reset() {
    return ResetCommandPool();
}

bool VulkanCommandContextBase::ResetAbandoned() {
    on_complete.clear();
    tracker.Reset();
    return Reset();
}

VulkanCommandContext::VulkanCommandContext(VulkanDevice* _device, EQueueType _type) :
    VulkanCommandContextBase(_device, _type),
    buffer_allocator(*_device),
    queries(*_device, _type) {}

void VulkanCommandContext::EnsureQueryCapacity(size_t _timestamp_slot_count, size_t _occlusion_pair_count) {
    queries.EnsureQueryCapacity(_timestamp_slot_count, _occlusion_pair_count);
}

void VulkanCommandContext::ConfigureTimestampCapabilityForRecording(
    uint32                         _valid_bits,
    EVulkanTimestampQueryResetMode _reset_mode
) {
    queries.ConfigureTimestampCapabilityForRecording(_valid_bits, _reset_mode);
}

void VulkanCommandContext::RecordQuery(VulkanCmdList& _cmd_list, const QueryCmd& _cmd) {
    queries.RecordQuery(_cmd_list, _cmd);
}

VkResult
VulkanCommandContext::PrepareQueriesAfterGpuCompletion(const VulkanOperationContext& _context) noexcept {
    return queries.PrepareQueriesAfterGpuCompletion(_context);
}

bool VulkanCommandContext::CompleteSuccessCallbacks() noexcept {
    return InvokeCompletionCallbacksNoexcept(on_complete, "command context");
}

void VulkanCommandContext::PublishQueriesAfterGpuCompletion(
    QueryPublishBatch _batch,
    bool              _gpu_success,
    std::string_view  _failure_reason
) noexcept {
    queries.PublishQueriesAfterGpuCompletion(_batch, _gpu_success, _failure_reason);
}

void VulkanCommandContext::NotifyQueriesAfterGpuCompletion(QueryPublishBatch _batch) noexcept {
    queries.NotifyQueriesAfterGpuCompletion(_batch);
}

bool VulkanCommandContext::CompleteSuccess() noexcept {
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

bool VulkanCommandContext::Reset() {
    if (!ResetCommandPool()) {
        return false;
    }
    buffer_allocator.Reset();
    queries.ClearRecords();
    return true;
}

// VulkanPresentContext
VulkanPresentContext::VulkanPresentContext(VulkanDevice* _device, EQueueType _type) :
    VulkanCommandContextBase(_device, _type) {
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
        throw std::runtime_error("failed to create Vulkan present context image-ready semaphore");
    }
    try {
        m_device->SetResourceName(
            uint64(image_ready_semaphore),
            VK_OBJECT_TYPE_SEMAPHORE,
            "PresentContextImageReadySemaphore_" + std::to_string(reinterpret_cast<uintptr_t>(this))
        );
    } catch (...) {
        vkDestroySemaphore(m_device->GetDevice(), image_ready_semaphore, VK_NULL_HANDLE);
        image_ready_semaphore = VK_NULL_HANDLE;
        throw;
    }
}

VulkanPresentContext::~VulkanPresentContext() {
    if (image_ready_semaphore != VK_NULL_HANDLE) {
        vkDestroySemaphore(m_device->GetDevice(), image_ready_semaphore, VK_NULL_HANDLE);
        image_ready_semaphore = VK_NULL_HANDLE;
    }
}

bool VulkanPresentContext::CompleteSuccess() noexcept {
    return InvokeCompletionCallbacksNoexcept(on_complete, "present context");
}

} // namespace Moer::Render
