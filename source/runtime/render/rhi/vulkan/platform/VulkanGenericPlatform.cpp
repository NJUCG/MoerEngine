//
// Created by 74535 on 2023/10/11.
//

#include "VulkanGenericPlatform.h"

#include "../VulkanDeviceFeature.h"

namespace Moer::Render {

void VulkanGenericPlatform::RestrictEnabledPhysicalDeviceFeatures(VulkanDeviceFeatures* _gpu_features) {
    // Disable everything sparse-related
    _gpu_features->core_1_0.shaderResourceResidency = VK_FALSE;
    _gpu_features->core_1_0.shaderResourceMinLod    = VK_FALSE;
    _gpu_features->core_1_0.sparseBinding           = VK_FALSE;
    _gpu_features->core_1_0.sparseResidencyBuffer   = VK_FALSE;
    _gpu_features->core_1_0.sparseResidencyImage2D  = VK_FALSE;
    _gpu_features->core_1_0.sparseResidencyImage3D  = VK_FALSE;
    _gpu_features->core_1_0.sparseResidency2Samples = VK_FALSE;
    _gpu_features->core_1_0.sparseResidency4Samples = VK_FALSE;
    _gpu_features->core_1_0.sparseResidency8Samples = VK_FALSE;
    _gpu_features->core_1_0.sparseResidencyAliased  = VK_FALSE;
}

} // namespace Moer::Render
