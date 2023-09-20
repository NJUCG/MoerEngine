//
// Created by 17152 on 2023/9/20.
//
#include "misc/MacroUtils.h"
#include "vulkan/VulkanRHI.h"

std::string vk_layer = MACRO_STR(__ENGINE_NAME__)MACRO_STR(_VK_LAYER_PATH);
