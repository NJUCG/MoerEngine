//
// Created by 74535 on 2023/10/11.
//

#include <vector>
#include <string>
#include <vulkan/vulkan.h>

#ifndef VULKAN_TYPEDEFS_H
#define VULKAN_TYPEDEFS_H

using TExtensionArray      = std::vector<std::string>;
using TExtensionPropsArray = std::vector<VkExtensionProperties>;
using TLayerArray          = std::vector<std::string>;

#endif//VULKAN_TYPEDEFS_H
