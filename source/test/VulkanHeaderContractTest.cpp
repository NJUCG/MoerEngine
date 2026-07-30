// This order is intentional: it reproduces the conflict that occurs when
// vulkan_core.h declares function prototypes before Volk declares its entry
// point variables.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <vulkan/vulkan_core.h>

#ifndef VK_NO_PROTOTYPES
#error "Moer::Render must propagate VK_NO_PROTOTYPES to every consumer."
#endif

#include "rhi/RHIWindowSurface.h"
#include "rhi/vulkan/VulkanDebugCallback.h"
#include "rhi/vulkan/VulkanFault.h"
#include "rhi/vulkan/VulkanMemoryAllocator.h"
#include "rhi/vulkan/VulkanPlatform.h"
#include "rhi/vulkan/VulkanTypeDefs.h"

#include "platform/Platform.h"

#include <type_traits>

#if PLATFORM_WINDOWS
static_assert(std::is_base_of_v<Moer::Render::VulkanGenericPlatform, Moer::Render::VulkanPlatform>);
#endif

int main() {
    return VK_SUCCESS == 0 ? 0 : 1;
}
