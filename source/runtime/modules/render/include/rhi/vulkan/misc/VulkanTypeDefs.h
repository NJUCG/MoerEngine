//
// Created by 74535 on 2023/10/11.
//

#include <string>

#include <vector>
#include <unordered_map>

#include <vulkan/vulkan.h>

#ifndef VULKAN_TYPEDEFS_H
#define VULKAN_TYPEDEFS_H

using TExtensionArray             = std::vector<std::string>;
using TExtensionPropsArray        = std::vector<VkExtensionProperties>;
using TLayerArray                 = std::vector<std::string>;
using TQueueFamilyPropertiesArray = std::vector<VkQueueFamilyProperties>;
using TDescriptorSetLayout        = std::pair<VkDescriptorSetLayout, std::vector<VkDescriptorSetLayoutBinding>>;
using TDescriptorCountMap         = std::unordered_map<VkDescriptorType, uint32_t>;

#endif//VULKAN_TYPEDEFS_H
