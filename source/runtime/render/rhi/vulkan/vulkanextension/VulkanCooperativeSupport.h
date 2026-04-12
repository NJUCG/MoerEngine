/**
 * Cooperative support 辅助模块
 *
 * 1. 负责从 Vulkan core feature 中提取 cooperative 相关前置条件。
 * 2. 负责输出 cooperative 能力摘要日志。
 * 3. 这里只做状态整理与展示，不参与 extension 注册或 device 创建。
 */
#ifndef VULKAN_COOPERATIVE_SUPPORT_H
#define VULKAN_COOPERATIVE_SUPPORT_H

#include "../VulkanDeviceFeature.h"
#include "../VulkanDeviceProperty.h"
#include "VulkanExtension.h"

namespace Moer::Render {

// 从 core 1.1/1.2 feature 中提取 cooperative bundle 需要的前置能力。
void UpdateCooperativePrerequisites(
    const VulkanDeviceFeatures&     _core_features,
    VulkanOptionalDeviceExtensions& _optional_extensions
);

// 输出 cooperative 扩展、枚举结果和前置能力的摘要日志。
void LogCooperativeSupportSummary(
    const VulkanOptionalDeviceExtensions& _optional_extensions,
    const VulkanOptionalDeviceProperties& _optional_properties
);

} // namespace Moer::Render

#endif // VULKAN_COOPERATIVE_SUPPORT_H