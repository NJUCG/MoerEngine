#include "VulkanDeviceFeature.h"

#include <algorithm>

namespace Moer::Render {
namespace {

template<typename T>
void PrependFeature(VkPhysicalDeviceFeatures2& _root, T& _features) {
    if constexpr (requires {
                      _features.head;
                      _features.tail;
                  }) {
        _features.tail->pNext = _root.pNext;
        _root.pNext           = _features.head;
    } else {
        _features.pNext = _root.pNext;
        _root.pNext     = &_features;
    }
}

} // namespace

VulkanDeviceFeatures::VulkanDeviceFeatures(uint32_t _api_version) noexcept : api_version(_api_version) {
    RebuildFeatureChain();
}

VulkanDeviceFeatures::VulkanDeviceFeatures(const VulkanDeviceFeatures& _other) noexcept :
    api_version(_other.api_version),
    features2(_other.features2),
    core_1_1(_other.core_1_1),
    core_1_2(_other.core_1_2),
    core_1_3(_other.core_1_3) {
    RebuildFeatureChain();
    if (api_version >= VK_API_VERSION_1_1 && api_version < VK_API_VERSION_1_2) {
        core_1_1_nodes.WriteFrom(core_1_1);
    }
}

VulkanDeviceFeatures& VulkanDeviceFeatures::operator=(const VulkanDeviceFeatures& _other) noexcept {
    if (this != &_other) {
        api_version = _other.api_version;
        features2   = _other.features2;
        core_1_1    = _other.core_1_1;
        core_1_2    = _other.core_1_2;
        core_1_3    = _other.core_1_3;
        RebuildFeatureChain();
        if (api_version >= VK_API_VERSION_1_1 && api_version < VK_API_VERSION_1_2) {
            core_1_1_nodes.WriteFrom(core_1_1);
        }
    }
    return *this;
}

// Moving fixed-size feature values follows the same pointer rebuilding rules
// as copying, leaving both objects with valid chains.
VulkanDeviceFeatures::VulkanDeviceFeatures(VulkanDeviceFeatures&& _other) noexcept :
    VulkanDeviceFeatures(static_cast<const VulkanDeviceFeatures&>(_other)) {}

VulkanDeviceFeatures& VulkanDeviceFeatures::operator=(VulkanDeviceFeatures&& _other) noexcept {
    return operator=(static_cast<const VulkanDeviceFeatures&>(_other));
}

VulkanDeviceFeatures VulkanDeviceFeatures::BuildRequiredFeatures(uint32_t _api_version) {
    VulkanDeviceFeatures required_features(_api_version);

    // 1.0 features
    required_features.features2.features.samplerAnisotropy = VK_TRUE;
    required_features.features2.features.sampleRateShading = VK_TRUE;
    required_features.features2.features.depthClamp        = VK_TRUE;
    // 1.2 features
    if (_api_version >= VK_API_VERSION_1_2) {

        required_features.core_1_2.timelineSemaphore   = VK_TRUE;
        required_features.core_1_2.bufferDeviceAddress = VK_TRUE;
        // MARK: need fallback to non-bindless if not supported
        // descriptor indexing features
        required_features.core_1_2.descriptorIndexing = VK_TRUE;
        // uniform
        required_features.core_1_2.shaderUniformBufferArrayNonUniformIndexing    = VK_TRUE;
        required_features.core_1_2.descriptorBindingUniformBufferUpdateAfterBind = VK_TRUE;
        // sampler
        required_features.core_1_2.shaderSampledImageArrayNonUniformIndexing    = VK_TRUE;
        required_features.core_1_2.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
        // image
        required_features.core_1_2.shaderStorageImageArrayNonUniformIndexing    = VK_TRUE;
        required_features.core_1_2.descriptorBindingStorageImageUpdateAfterBind = VK_TRUE;
        // buffer
        required_features.core_1_2.shaderStorageBufferArrayNonUniformIndexing    = VK_TRUE;
        required_features.core_1_2.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
        required_features.core_1_2.drawIndirectCount                             = VK_TRUE;
    }

    // 1.3 features
    if (_api_version >= VK_API_VERSION_1_3) {

        required_features.core_1_3.synchronization2 = VK_TRUE;
        required_features.core_1_3.dynamicRendering = VK_TRUE;
    }

    return required_features;
}

VulkanDeviceFeatures
VulkanDeviceFeatures::QuerySupportedFeatures(VkPhysicalDevice _gpu, uint32_t _api_version) {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(_gpu, &properties);
    const uint32_t       query_api_version = std::min(_api_version, properties.apiVersion);
    VulkanDeviceFeatures features(query_api_version);
    if (query_api_version >= VK_API_VERSION_1_2) {
        vkGetPhysicalDeviceFeatures2(_gpu, &features.features2);
    } else if (query_api_version >= VK_API_VERSION_1_1) {
        vkGetPhysicalDeviceFeatures2(_gpu, &features.features2);
        features.core_1_1_nodes.WriteInto(features.core_1_1);
    } else {
        vkGetPhysicalDeviceFeatures(_gpu, &features.features2.features);
    }
    return features;
}

void VulkanDeviceFeatures::RebuildFeatureChain() {
    features2.sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext            = nullptr;
    core_1_1.sType             = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    core_1_2.sType             = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    core_1_3.sType             = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    core_1_1.pNext             = nullptr;
    core_1_2.pNext             = nullptr;
    core_1_3.pNext             = nullptr;
    core_1_1_nodes.tail->pNext = nullptr;

    if (api_version >= VK_API_VERSION_1_3) {
        PrependFeature(features2, core_1_3);
        PrependFeature(features2, core_1_2);
        PrependFeature(features2, core_1_1);
    } else if (api_version >= VK_API_VERSION_1_2) {
        PrependFeature(features2, core_1_2);
        PrependFeature(features2, core_1_1);
    } else if (api_version >= VK_API_VERSION_1_1) {
        // VkPhysicalDeviceVulkan11Features groups 1.1 capabilities but was introduced
        // in Vulkan 1.2. Use the individual feature structures for the 1.1 path.
        // https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceVulkan11Features.html
        PrependFeature(features2, core_1_1_nodes);
    } else {
        // Do nothing, only core 1.0 features are available.
    }
}

void VulkanDeviceFeatures::Vulkan11FeatureNodes::WriteFrom(const VkPhysicalDeviceVulkan11Features& _features
) {
    storage_16bit.storageBuffer16BitAccess           = _features.storageBuffer16BitAccess;
    storage_16bit.uniformAndStorageBuffer16BitAccess = _features.uniformAndStorageBuffer16BitAccess;
    storage_16bit.storagePushConstant16              = _features.storagePushConstant16;
    storage_16bit.storageInputOutput16               = _features.storageInputOutput16;
    multiview.multiview                              = _features.multiview;
    multiview.multiviewGeometryShader                = _features.multiviewGeometryShader;
    multiview.multiviewTessellationShader            = _features.multiviewTessellationShader;
    variable_pointers.variablePointersStorageBuffer  = _features.variablePointersStorageBuffer;
    variable_pointers.variablePointers               = _features.variablePointers;
    protected_memory.protectedMemory                 = _features.protectedMemory;
    sampler_ycbcr.samplerYcbcrConversion             = _features.samplerYcbcrConversion;
    shader_draw_parameters.shaderDrawParameters      = _features.shaderDrawParameters;
}

void VulkanDeviceFeatures::Vulkan11FeatureNodes::WriteInto(VkPhysicalDeviceVulkan11Features& _features) const {
    _features.storageBuffer16BitAccess           = storage_16bit.storageBuffer16BitAccess;
    _features.uniformAndStorageBuffer16BitAccess = storage_16bit.uniformAndStorageBuffer16BitAccess;
    _features.storagePushConstant16              = storage_16bit.storagePushConstant16;
    _features.storageInputOutput16               = storage_16bit.storageInputOutput16;
    _features.multiview                          = multiview.multiview;
    _features.multiviewGeometryShader            = multiview.multiviewGeometryShader;
    _features.multiviewTessellationShader        = multiview.multiviewTessellationShader;
    _features.variablePointersStorageBuffer      = variable_pointers.variablePointersStorageBuffer;
    _features.variablePointers                   = variable_pointers.variablePointers;
    _features.protectedMemory                    = protected_memory.protectedMemory;
    _features.samplerYcbcrConversion             = sampler_ycbcr.samplerYcbcrConversion;
    _features.shaderDrawParameters               = shader_draw_parameters.shaderDrawParameters;
}

void VulkanDeviceFeatures::AttachToDeviceCreateInfo(VkDeviceCreateInfo& _device_create_info) {
    RebuildFeatureChain();
    if (api_version < VK_API_VERSION_1_1) {
        _device_create_info.pEnabledFeatures = &features2.features;
        return;
    }

    void* extension_chain = const_cast<void*>(_device_create_info.pNext);
    if (api_version >= VK_API_VERSION_1_3) {
        core_1_3.pNext = extension_chain;
    } else if (api_version >= VK_API_VERSION_1_2) {
        core_1_2.pNext = extension_chain;
    } else {
        core_1_1_nodes.WriteFrom(core_1_1);
        core_1_1_nodes.tail->pNext = extension_chain;
    }
    _device_create_info.pEnabledFeatures = nullptr;
    _device_create_info.pNext            = &features2;
}

} // namespace Moer::Render
