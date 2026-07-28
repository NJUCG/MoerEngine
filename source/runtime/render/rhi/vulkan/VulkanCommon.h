#pragma once

// MoerEngine uses Volk for every Vulkan entry point. Include this header
// instead of Vulkan SDK headers so platform declarations and
// VK_NO_PROTOTYPES are established before Vulkan is parsed.
#if defined(_WIN32)
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR 1
#endif
#elif defined(__linux__)
#ifndef VK_USE_PLATFORM_XLIB_KHR
#define VK_USE_PLATFORM_XLIB_KHR 1
#endif
#endif

#if (defined(VULKAN_H_) || defined(VULKAN_CORE_H_)) && !defined(VK_NO_PROTOTYPES)
#error "Include VulkanCommon.h before Vulkan SDK headers, or compile with VK_NO_PROTOTYPES."
#endif

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES 1
#endif

#include <cstdint>
#include <volk.h>

namespace Moer::Render {

enum class EVulkanTimestampQueryResetMode : std::uint8_t {
    Unsupported,
    CommandBuffer,
    Host,
};

} // namespace Moer::Render
