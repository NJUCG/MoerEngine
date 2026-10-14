#ifndef VULKAN_DEVICE_FEATURE_H
#define VULKAN_DEVICE_FEATURE_H

#include "VulkanCommon.h"

namespace Moer::Render {

class VulkanDeviceFeatures {
public:
    explicit VulkanDeviceFeatures(uint32_t _api_version = VK_API_VERSION_1_0) noexcept;
    VulkanDeviceFeatures(const VulkanDeviceFeatures& _other) noexcept;
    VulkanDeviceFeatures& operator=(const VulkanDeviceFeatures& _other) noexcept;
    VulkanDeviceFeatures(VulkanDeviceFeatures&& _other) noexcept;
    VulkanDeviceFeatures& operator=(VulkanDeviceFeatures&& _other) noexcept;

    static VulkanDeviceFeatures QuerySupportedFeatures(VkPhysicalDevice _gpu, uint32_t _api_version);

    static VulkanDeviceFeatures BuildRequiredFeatures(uint32_t _api_version);

    bool SupportsAll(const VulkanDeviceFeatures& _required_features) const;

    const VkPhysicalDeviceFeatures2& GetFeatures2() const {
        return features2;
    }

    const VkPhysicalDeviceFeatures& GetCore10Features() const {
        return features2.features;
    }

    const VkPhysicalDeviceVulkan11Features& GetCore11Features() const {
        return core_1_1;
    }

    const VkPhysicalDeviceVulkan12Features& GetCore12Features() const {
        return core_1_2;
    }

    const VkPhysicalDeviceVulkan13Features& GetCore13Features() const {
        return core_1_3;
    }

    // Attach a local copy that stays alive through vkCreateDevice. The copy
    // borrows the existing extension chain; cached features keep internal links.
    void AttachToDeviceCreateInfo(VkDeviceCreateInfo& _device_create_info);

private:
    void RebuildFeatureChain();
    void InitializeVulkan11Chain();
    void ReadVulkan11Features();

    // Copies and moves rebuild pNext links to this object's own members.
    uint32_t                         api_version;
    VkPhysicalDeviceFeatures2        features2{};
    VkPhysicalDeviceVulkan11Features core_1_1{};
    VkPhysicalDeviceVulkan12Features core_1_2{};
    VkPhysicalDeviceVulkan13Features core_1_3{};

    // The 1.1 query path uses individual structures. core_1_1 provides the
    // same observation interface after their query results are read back.
    VkPhysicalDevice16BitStorageFeatures           storage_16bit{};
    VkPhysicalDeviceMultiviewFeatures              multiview{};
    VkPhysicalDeviceVariablePointersFeatures       variable_pointers{};
    VkPhysicalDeviceProtectedMemoryFeatures        protected_memory{};
    VkPhysicalDeviceSamplerYcbcrConversionFeatures sampler_ycbcr{};
    VkPhysicalDeviceShaderDrawParametersFeatures   shader_draw_parameters{};
};

} // namespace Moer::Render

#endif //VULKAN_DEVICE_FEATURE_H
