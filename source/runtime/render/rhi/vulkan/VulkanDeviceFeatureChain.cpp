#include "VulkanDeviceFeatureChain.h"

namespace Moer::Render {
namespace {

template<typename T>
void PrependFeature(VkPhysicalDeviceFeatures2& _root, T& _features) {
    _features.pNext = _root.pNext;
    _root.pNext     = &_features;
}

} // namespace

VulkanDeviceFeatureChain::VulkanDeviceFeatureChain(
    const VulkanDeviceFeatures& _features,
    uint32_t                    _api_version
) :
    api_version(_api_version),
    core_1_1(_features.GetCore11Features()),
    core_1_2(_features.GetCore12Features()),
    core_1_3(_features.GetCore13Features()) {
    features2.sType    = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.features = _features.GetCore10Features();
    core_1_1.sType     = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    core_1_2.sType     = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    core_1_3.sType     = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    core_1_1.pNext     = nullptr;
    core_1_2.pNext     = nullptr;
    core_1_3.pNext     = nullptr;

    if (api_version >= VK_API_VERSION_1_3) {
        PrependFeature(features2, core_1_3);
    }
    if (api_version >= VK_API_VERSION_1_2) {
        PrependFeature(features2, core_1_2);
        PrependFeature(features2, core_1_1);
    } else if (api_version >= VK_API_VERSION_1_1) {
        InitializeVulkan11Chain(core_1_1);
    }
}

void VulkanDeviceFeatureChain::InitializeVulkan11Chain(const VkPhysicalDeviceVulkan11Features& _features) {
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

    PrependFeature(features2, shader_draw_parameters);
    PrependFeature(features2, sampler_ycbcr);
    PrependFeature(features2, protected_memory);
    PrependFeature(features2, variable_pointers);
    PrependFeature(features2, multiview);
    PrependFeature(features2, storage_16bit);
}

void VulkanDeviceFeatureChain::CaptureVulkan11Features(VkPhysicalDeviceVulkan11Features& _features) const {
    _features.sType                              = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
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

VulkanDeviceFeatures VulkanDeviceFeatureChain::CaptureFeatures() const {
    VulkanDeviceFeatures result;
    result.core_1_0 = features2.features;
    if (api_version >= VK_API_VERSION_1_2) {
        result.core_1_1 = core_1_1;
        result.core_1_2 = core_1_2;
    } else if (api_version >= VK_API_VERSION_1_1) {
        CaptureVulkan11Features(result.core_1_1);
    }
    if (api_version >= VK_API_VERSION_1_3) {
        result.core_1_3 = core_1_3;
    }
    // Snapshots retain feature values, never pointers into this transient chain.
    result.core_1_1.pNext = nullptr;
    result.core_1_2.pNext = nullptr;
    result.core_1_3.pNext = nullptr;
    return result;
}

void VulkanDeviceFeatureChain::AttachToDeviceCreateInfo(VkDeviceCreateInfo& _device_create_info) {
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
        shader_draw_parameters.pNext = extension_chain;
    }
    _device_create_info.pEnabledFeatures = nullptr;
    _device_create_info.pNext            = &features2;
}

} // namespace Moer::Render
