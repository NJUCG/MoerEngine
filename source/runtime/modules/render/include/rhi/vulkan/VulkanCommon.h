#ifndef VULKAN_COMMON_H
#define VULKAN_COMMON_H

#include <vulkan.h>

#include <vector>

namespace MoerEngine {
namespace RHI {
namespace Vulkan {

namespace Util {
    struct SwapChainSupportDetails {
        VkSurfaceCapabilitiesKHR        capabilities;
        std::vector<VkSurfaceFormatKHR> formats;
        std::vector<VkPresentModeKHR>   present_modes;
    };
}

}
}
}// namespace MoerEngine::RHI::Vulkan::Util
#endif// !VULKAN_COMMON_H
