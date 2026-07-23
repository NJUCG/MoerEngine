//
// Created by 74535 on 2023/10/11.
//

#ifndef VULKAN_TYPEDEFS_H
#define VULKAN_TYPEDEFS_H

#include "VulkanCommon.h"
#include "misc/STL.h"

namespace Moer::Render {
class VulkanDeviceExtension;

using TVulkanDeviceExtensionArray      = Moer::Array<std::shared_ptr<VulkanDeviceExtension>>;
using TExtensionArray                  = Moer::Array<std::string_view>;
using TLayerArray                      = Moer::Array<std::string_view>;
using TQueueFamilyPropertiesArray      = Moer::Array<VkQueueFamilyProperties>;
using TDescriptorSetLayoutBindingArray = Moer::Array<VkDescriptorSetLayoutBinding>;
using TDescriptorCountMap              = Moer::UnorderedMap<VkDescriptorType, uint32_t>;
} // namespace Moer::Render
#endif //VULKAN_TYPEDEFS_H
