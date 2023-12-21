//
// Created by 74535 on 2023/10/11.
//

#ifndef VULKAN_TYPEDEFS_H
#define VULKAN_TYPEDEFS_H

#include "misc/STL.h"

#include <vulkan/vulkan_core.h>

class VulkanDeviceExtension;

using TVulkanDeviceExtensionArray = Moer::Array<std::unique_ptr<VulkanDeviceExtension>>;
using TExtensionArray             = Moer::Array<std::string>;
using TExtensionPropsArray        = Moer::Array<VkExtensionProperties>;
using TLayerArray                 = Moer::Array<std::string>;
using TQueueFamilyPropertiesArray = Moer::Array<VkQueueFamilyProperties>;
using TDescriptorSetLayoutInfo    = std::pair<VkDescriptorSetLayout, Moer::Array<VkDescriptorSetLayoutBinding>>;
using TDescriptorCountMap         = Moer::UnorderedMap<VkDescriptorType, uint32_t>;

#endif//VULKAN_TYPEDEFS_H
