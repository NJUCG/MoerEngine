//
// Created by 74535 on 2023/10/1.
//

#ifndef VULKAN_UTIL_H
#define VULKAN_UTIL_H

#include "misc/STL.h"

#include "VulkanPlatform.h"

namespace Moer { namespace RHI { namespace Vulkan { namespace Util {
struct SwapChainSupportDetails {
    VkSurfaceCapabilitiesKHR        capabilities{};
    Moer::Array<VkSurfaceFormatKHR> formats;
    Moer::Array<VkPresentModeKHR>   present_modes;
};

enum class ESwapChainSupportQueryStatus : uint8_t {
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
QuerySwapChainSupport(VkPhysicalDevice _gpu, VkSurfaceKHR _surface, uint32_t _present_queue_family);

uint32_t MemCrc32(const void* data, size_t data_size);
}}}} // namespace Moer::RHI::Vulkan::Util

#endif // !VULKAN_UTIL_H
