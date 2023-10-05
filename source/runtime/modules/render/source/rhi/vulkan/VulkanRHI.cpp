#include "misc/MacroUtils.h"
#include "rhi/vulkan/VulkanRHI.h"

#include <string>
#include <vulkan.h>

std::string vk_layer = MACRO_STR(__ENGINE_NAME__)MACRO_STR(_VK_LAYER_PATH);
