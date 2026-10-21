#pragma once

#include "RenderAPI.h"
#include "VulkanCommon.h"
#include "rhi/RHICommon.h"

#include <cstdint>
#include <string_view>

namespace Moer::Render {

enum class EVulkanFaultOperation : uint8_t {
    None,
    QueueSubmit,
    QueueSubmitEmpty,
    PresentSubmit,
    QueuePresent,
    AcquireNextImage,
    TimelineHostWait,
    CommandPoolCreate,
    CommandBufferAllocate,
    CommandPoolReset,
    QueryPoolCreate,
    QueryPoolResults,
    SwapchainSurfaceCreate,
    SwapchainSurfaceQuery,
    SwapchainCreate,
    SwapchainGetImages,
    SwapchainSemaphoreCreate,
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

// Operation classification only; accepted work and GPU completion are tracked separately.
struct VulkanOperationResult {
    EVulkanOperationStatus status{EVulkanOperationStatus::Success};
    VkResult               result{VK_SUCCESS};

    [[nodiscard]] bool Succeeded() const {
        return status == EVulkanOperationStatus::Success;
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

[[nodiscard]] RENDER_API std::string_view VulkanFaultOperationName(EVulkanFaultOperation _operation);
[[nodiscard]] RENDER_API std::string_view VulkanQueueName(EQueueType _type);
[[nodiscard]] RENDER_API std::string_view VulkanResultName(VkResult _result);

} // namespace Moer::Render
