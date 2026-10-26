#include "VulkanFault.h"

#include "vulkan/vk_enum_string_helper.h"

namespace Moer::Render {

std::string_view VulkanFaultOperationName(EVulkanFaultOperation _operation) {
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
        case EVulkanFaultOperation::TimelineQuery:
            return "TimelineQuery";
        case EVulkanFaultOperation::CommandPoolCreate:
            return "CommandPoolCreate";
        case EVulkanFaultOperation::CommandBufferAllocate:
            return "CommandBufferAllocate";
        case EVulkanFaultOperation::CommandPoolReset:
            return "CommandPoolReset";
        case EVulkanFaultOperation::QueryPoolCreate:
            return "QueryPoolCreate";
        case EVulkanFaultOperation::QueryPoolResults:
            return "QueryPoolResults";
        case EVulkanFaultOperation::SwapchainSurfaceCreate:
            return "SwapchainSurfaceCreate";
        case EVulkanFaultOperation::SwapchainSurfaceQuery:
            return "SwapchainSurfaceQuery";
        case EVulkanFaultOperation::SwapchainCreate:
            return "SwapchainCreate";
        case EVulkanFaultOperation::SwapchainGetImages:
            return "SwapchainGetImages";
        case EVulkanFaultOperation::SwapchainSemaphoreCreate:
            return "SwapchainSemaphoreCreate";
        case EVulkanFaultOperation::DeviceWaitIdle:
            return "DeviceWaitIdle";
        default:
            return "None";
    }
}

std::string_view VulkanQueueName(EQueueType _type) {
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

std::string_view VulkanResultName(VkResult _result) {
    return string_VkResult(_result);
}

} // namespace Moer::Render
