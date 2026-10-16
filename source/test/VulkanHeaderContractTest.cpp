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
#include "rhi/vulkan/VulkanCommandContext.h"
#include "rhi/vulkan/VulkanCommandQueries.h"
#include "rhi/vulkan/VulkanDebugCallback.h"
#include "rhi/vulkan/VulkanDeviceFeature.h"
#include "rhi/vulkan/VulkanFault.h"
#include "rhi/vulkan/VulkanMemoryAllocator.h"
#include "rhi/vulkan/VulkanPlatform.h"
#include "rhi/vulkan/VulkanTemporaryBufferAllocator.h"
#include "rhi/vulkan/VulkanTypeDefs.h"
#include "window/WindowFrameSnapshot.h"

#include "platform/Platform.h"

#include <type_traits>

#if PLATFORM_WINDOWS
static_assert(std::is_base_of_v<Moer::Render::VulkanGenericPlatform, Moer::Render::VulkanPlatform>);
#endif

// Command resources own native handles and cannot be copied or relocated while
// command lists retain pointers to their pool.
static_assert(!std::is_copy_constructible_v<Moer::Render::VulkanCommandPool>);
static_assert(!std::is_copy_assignable_v<Moer::Render::VulkanCommandPool>);
static_assert(!std::is_move_constructible_v<Moer::Render::VulkanCommandPool>);
static_assert(!std::is_copy_constructible_v<Moer::Render::VulkanCmdList>);
static_assert(!std::is_move_constructible_v<Moer::Render::VulkanCmdList>);
static_assert(!std::is_copy_constructible_v<Moer::Render::VulkanCommandContextBase>);
static_assert(!std::is_move_constructible_v<Moer::Render::VulkanCommandContextBase>);
static_assert(!std::is_copy_constructible_v<Moer::Render::VulkanPresentContext>);
static_assert(!std::is_copy_constructible_v<Moer::Render::VulkanCommandContext>);
static_assert(!std::is_move_constructible_v<Moer::Render::VulkanCommandContext>);
static_assert(!std::is_copy_constructible_v<Moer::Render::VulkanTemporaryBufferAllocator>);
static_assert(!std::is_move_constructible_v<Moer::Render::VulkanTemporaryBufferAllocator>);

static_assert(!std::is_copy_constructible_v<Moer::Render::VkNativeQueryPool>);
static_assert(!std::is_copy_constructible_v<Moer::Render::VulkanCommandQueries>);
static_assert(!std::is_move_constructible_v<Moer::Render::VulkanCommandQueries>);

int main() {
    return VK_SUCCESS == 0 ? 0 : 1;
}
