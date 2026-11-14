/**
 * Cooperative support 辅助模块
 *
 * 输出已启用的 cooperative 能力和模式表，不参与设备初始化。
 */
#ifndef VULKAN_COOPERATIVE_SUPPORT_H
#define VULKAN_COOPERATIVE_SUPPORT_H

#include "rhi/plugin/RHICooperative.h"

#include "../VulkanDeviceFeature.h"
#include "../VulkanDeviceProperty.h"

namespace Moer::Render {

// 输出 cooperative 扩展、枚举结果和前置能力的摘要日志。
void LogCooperativeSupportSummary(
    const VulkanDeviceCapabilities& _optional_extensions,
    const VulkanOptionalDeviceProperties& _optional_properties
);

CooperativeExtensionInfo BuildCooperativeExtensionInfo(
    const VulkanDeviceCapabilities& _optional_extensions,
    const VulkanOptionalDeviceProperties& _optional_properties
);

} // namespace Moer::Render

#endif // VULKAN_COOPERATIVE_SUPPORT_H
