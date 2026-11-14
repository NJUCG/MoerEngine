#pragma once

// Moer::VulkanHeaders supplies the Vulkan compile definitions before any SDK
// header is parsed. Keep the backend's native declarations and entry points here.
#ifndef VK_NO_PROTOTYPES
#error "Link Moer::VulkanHeaders to supply VK_NO_PROTOTYPES and Vulkan platform definitions."
#endif

#include "misc/Types.h"
#include <volk.h>

namespace Moer::Render {

inline constexpr uint32 k_vulkan_api_version = VK_API_VERSION_1_3;

} // namespace Moer::Render
