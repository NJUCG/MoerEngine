//
// Created by 74535 on 2023/10/1.
//
#include "VulkanWindowsPlatform.h"

namespace Moer::Render {
void VulkanWindowsPlatform::GetInstanceLayers(TLayerArray& _layers) {
#ifndef NDEBUG
    _layers.emplace_back("VK_LAYER_KHRONOS_validation");
#endif
}
} // namespace Moer::Render
