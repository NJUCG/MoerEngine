#ifndef VULKAN_EXTENSION_REGISTRY_H
#define VULKAN_EXTENSION_REGISTRY_H

#include <span>

#include "VulkanExtension.h"

namespace Moer::Render {

std::span<const VulkanExtensionDesc> GetVulkanExtensionDescs();

} // namespace Moer::Render

#endif // VULKAN_EXTENSION_REGISTRY_H