#pragma once

#include "rhi/RHICommon.h"
#include "vulkan/vulkan_core.h"

#include <cstdint>
#include <string_view>

namespace Moer::Render {

enum class EVulkanFaultPublishState : uint8_t {
    Healthy,
    Publishing,
    Faulted
};

enum class EVulkanFaultOperation : uint8_t {
    None,
    QueueSubmit,
    QueueSubmitEmpty,
    PresentSubmit,
    QueuePresent,
    AcquireNextImage,
    TimelineHostWait,
    PresentFenceWait,
    PresentFenceReset,
    CommandPoolReset,
    SwapchainCreate,
    SwapchainGetImages,
    SwapchainSemaphoreCreate,
    SwapchainFenceCreate,
    QueueWaitIdle,
    DeviceWaitIdle
};

enum class EVulkanOperationStatus : uint8_t {
    Success,
    Retry,
    Recreate,
    Faulted,
    Rejected
};

struct VulkanOperationContext {
    EVulkanFaultOperation operation{EVulkanFaultOperation::None};
    EQueueType            queue_type{EQueueType::Ignore};
    VkQueue               queue{VK_NULL_HANDLE};
    uint64_t              timeline{0};
    uint64_t              work_serial{0};
};

struct VulkanOperationResult {
    EVulkanOperationStatus status{EVulkanOperationStatus::Success};
    VkResult               result{VK_SUCCESS};
    bool                   injected{false};
    bool                   predrained{false};

    [[nodiscard]] bool Succeeded() const {
        return status == EVulkanOperationStatus::Success;
    }

    [[nodiscard]] bool WasSubmitted() const {
        return Succeeded();
    }
};

struct VulkanFaultRecord {
    EVulkanFaultOperation operation{EVulkanFaultOperation::None};
    VkResult              result{VK_SUCCESS};
    EQueueType            queue_type{EQueueType::Ignore};
    uint64_t              queue_handle{0};
    uint64_t              timeline{0};
    uint64_t              work_serial{0};
    uint32_t              thread_id{0};
    bool                  injected{false};
    bool                  predrained{false};
};

[[nodiscard]] inline std::string_view VulkanFaultOperationName(EVulkanFaultOperation _operation) {
    switch (_operation) {
        case EVulkanFaultOperation::QueueSubmit:
            return "QueueSubmit";
        case EVulkanFaultOperation::QueueSubmitEmpty:
            return "QueueSubmitEmpty";
        case EVulkanFaultOperation::PresentSubmit:
            return "PresentSubmit";
        case EVulkanFaultOperation::QueuePresent:
            return "QueuePresent";
        case EVulkanFaultOperation::AcquireNextImage:
            return "AcquireNextImage";
        case EVulkanFaultOperation::TimelineHostWait:
            return "TimelineHostWait";
        case EVulkanFaultOperation::PresentFenceWait:
            return "PresentFenceWait";
        case EVulkanFaultOperation::PresentFenceReset:
            return "PresentFenceReset";
        case EVulkanFaultOperation::CommandPoolReset:
            return "CommandPoolReset";
        case EVulkanFaultOperation::SwapchainCreate:
            return "SwapchainCreate";
        case EVulkanFaultOperation::SwapchainGetImages:
            return "SwapchainGetImages";
        case EVulkanFaultOperation::SwapchainSemaphoreCreate:
            return "SwapchainSemaphoreCreate";
        case EVulkanFaultOperation::SwapchainFenceCreate:
            return "SwapchainFenceCreate";
        case EVulkanFaultOperation::QueueWaitIdle:
            return "QueueWaitIdle";
        case EVulkanFaultOperation::DeviceWaitIdle:
            return "DeviceWaitIdle";
        default:
            return "None";
    }
}

[[nodiscard]] inline std::string_view VulkanQueueName(EQueueType _type) {
    switch (_type) {
        case EQueueType::Graphics:
            return "Graphics";
        case EQueueType::Compute:
            return "Compute";
        case EQueueType::Copy:
            return "Copy";
        default:
            return "Ignore";
    }
}

[[nodiscard]] inline std::string_view VulkanResultName(VkResult _result) {
    switch (_result) {
        case VK_SUCCESS:
            return "VK_SUCCESS";
        case VK_NOT_READY:
            return "VK_NOT_READY";
        case VK_TIMEOUT:
            return "VK_TIMEOUT";
        case VK_SUBOPTIMAL_KHR:
            return "VK_SUBOPTIMAL_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR:
            return "VK_ERROR_OUT_OF_DATE_KHR";
        case VK_ERROR_SURFACE_LOST_KHR:
            return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT:
            return "VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT";
        case VK_ERROR_OUT_OF_HOST_MEMORY:
            return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY:
            return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_DEVICE_LOST:
            return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_VALIDATION_FAILED_EXT:
            return "VK_ERROR_VALIDATION_FAILED_EXT";
        case VK_ERROR_UNKNOWN:
            return "VK_ERROR_UNKNOWN";
        default:
            return "VK_RESULT_UNKNOWN";
    }
}

} // namespace Moer::Render
