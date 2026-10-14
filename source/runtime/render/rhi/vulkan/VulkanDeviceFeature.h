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
    struct Vulkan11FeatureNodes {
        void WriteFrom(const VkPhysicalDeviceVulkan11Features& _features);
        void WriteInto(VkPhysicalDeviceVulkan11Features& _features) const;

        VkPhysicalDevice16BitStorageFeatures storage_16bit{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES,
            &multiview
        };
        VkPhysicalDeviceMultiviewFeatures multiview{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES,
            &variable_pointers
        };
        VkPhysicalDeviceVariablePointersFeatures variable_pointers{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES,
            &protected_memory
        };
        VkPhysicalDeviceProtectedMemoryFeatures protected_memory{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES,
            &sampler_ycbcr
        };
        VkPhysicalDeviceSamplerYcbcrConversionFeatures sampler_ycbcr{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES,
            &shader_draw_parameters
        };
        VkPhysicalDeviceShaderDrawParametersFeatures shader_draw_parameters{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES
        };

        VkPhysicalDevice16BitStorageFeatures* const         head = &storage_16bit;
        VkPhysicalDeviceShaderDrawParametersFeatures* const tail = &shader_draw_parameters;
    };

    void RebuildFeatureChain();

    // Copies and moves rebuild pNext links to this object's own members.
    uint32_t                         api_version;
    VkPhysicalDeviceFeatures2        features2{};
    VkPhysicalDeviceVulkan11Features core_1_1{};
    VkPhysicalDeviceVulkan12Features core_1_2{};
    VkPhysicalDeviceVulkan13Features core_1_3{};

    // Each object initializes its own node links. Copies transfer feature values
    // through core_1_1; the compatibility nodes never borrow another object's links.
    Vulkan11FeatureNodes core_1_1_nodes{};
};

} // namespace Moer::Render

#endif //VULKAN_DEVICE_FEATURE_H
