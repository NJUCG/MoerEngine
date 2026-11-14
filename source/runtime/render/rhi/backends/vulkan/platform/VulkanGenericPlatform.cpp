//
// Created by 74535 on 2023/10/11.
//

#include "VulkanGenericPlatform.h"

namespace Moer::Render {

void VulkanGenericPlatform::RestrictEnabledPhysicalDeviceFeatures(VkPhysicalDeviceFeatures& _features) {
    // Disable everything sparse-related
    _features.shaderResourceResidency = VK_FALSE;
    _features.shaderResourceMinLod    = VK_FALSE;
    _features.sparseBinding           = VK_FALSE;
    _features.sparseResidencyBuffer   = VK_FALSE;
    _features.sparseResidencyImage2D  = VK_FALSE;
    _features.sparseResidencyImage3D  = VK_FALSE;
    _features.sparseResidency2Samples = VK_FALSE;
    _features.sparseResidency4Samples = VK_FALSE;
    _features.sparseResidency8Samples = VK_FALSE;
    _features.sparseResidencyAliased  = VK_FALSE;
}

} // namespace Moer::Render
