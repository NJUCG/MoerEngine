//
// Created by 74535 on 2023/10/1.
//

#ifndef VULKAN_SURFACE_SUPPORT_H
#define VULKAN_SURFACE_SUPPORT_H

#include "misc/STL.h"
#include "misc/Types.h"

#include "VulkanCommon.h"

namespace Moer::Render {
struct SwapChainSupportDetails {
    VkSurfaceCapabilitiesKHR        capabilities{};
    Moer::Array<VkSurfaceFormatKHR> formats;
    Moer::Array<VkPresentModeKHR>   present_modes;
};

enum class ESwapChainSupportQueryStatus : uint8 {
    Success,
    NativeFailure,
    PresentUnsupported,
    NoSurfaceFormats,
    NoPresentModes,
    EnumerationIncomplete,
};

struct SwapChainSupportQueryResult {
    ESwapChainSupportQueryStatus status{ESwapChainSupportQueryStatus::NativeFailure};
    VkResult                     native_result{VK_ERROR_INITIALIZATION_FAILED};
    SwapChainSupportDetails      details{};

    [[nodiscard]] bool Succeeded() const noexcept {
        return status == ESwapChainSupportQueryStatus::Success && native_result == VK_SUCCESS;
    }
};

// Vulkan backend-internal query contract. This is intentionally not exported
// from moer_render; external RHI consumers use SwapchainCreateInfo instead.
SwapChainSupportQueryResult
QuerySwapChainSupport(VkPhysicalDevice _gpu, VkSurfaceKHR _surface, uint32 _present_queue_family);

} // namespace Moer::Render

#endif // !VULKAN_SURFACE_SUPPORT_H
