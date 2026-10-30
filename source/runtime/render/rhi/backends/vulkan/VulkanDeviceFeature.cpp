#include "VulkanDeviceFeature.h"
#include "misc/Types.h"

#include <algorithm>
#include <array>

namespace Moer::Render {
namespace {

template<typename T>
void PrependFeature(void*& _chain_head, T& _features) {
    if constexpr (requires {
                      _features.head;
                      _features.tail;
                  }) {
        _features.tail->pNext = _chain_head;
        _chain_head           = _features.head;
    } else {
        _features.pNext = _chain_head;
        _chain_head     = &_features;
    }
}

constexpr std::array core_1_0_members{
    &VkPhysicalDeviceFeatures::robustBufferAccess,
    &VkPhysicalDeviceFeatures::fullDrawIndexUint32,
    &VkPhysicalDeviceFeatures::imageCubeArray,
    &VkPhysicalDeviceFeatures::independentBlend,
    &VkPhysicalDeviceFeatures::geometryShader,
    &VkPhysicalDeviceFeatures::tessellationShader,
    &VkPhysicalDeviceFeatures::sampleRateShading,
    &VkPhysicalDeviceFeatures::dualSrcBlend,
    &VkPhysicalDeviceFeatures::logicOp,
    &VkPhysicalDeviceFeatures::multiDrawIndirect,
    &VkPhysicalDeviceFeatures::drawIndirectFirstInstance,
    &VkPhysicalDeviceFeatures::depthClamp,
    &VkPhysicalDeviceFeatures::depthBiasClamp,
    &VkPhysicalDeviceFeatures::fillModeNonSolid,
    &VkPhysicalDeviceFeatures::depthBounds,
    &VkPhysicalDeviceFeatures::wideLines,
    &VkPhysicalDeviceFeatures::largePoints,
    &VkPhysicalDeviceFeatures::alphaToOne,
    &VkPhysicalDeviceFeatures::multiViewport,
    &VkPhysicalDeviceFeatures::samplerAnisotropy,
    &VkPhysicalDeviceFeatures::textureCompressionETC2,
    &VkPhysicalDeviceFeatures::textureCompressionASTC_LDR,
    &VkPhysicalDeviceFeatures::textureCompressionBC,
    &VkPhysicalDeviceFeatures::occlusionQueryPrecise,
    &VkPhysicalDeviceFeatures::pipelineStatisticsQuery,
    &VkPhysicalDeviceFeatures::vertexPipelineStoresAndAtomics,
    &VkPhysicalDeviceFeatures::fragmentStoresAndAtomics,
    &VkPhysicalDeviceFeatures::shaderTessellationAndGeometryPointSize,
    &VkPhysicalDeviceFeatures::shaderImageGatherExtended,
    &VkPhysicalDeviceFeatures::shaderStorageImageExtendedFormats,
    &VkPhysicalDeviceFeatures::shaderStorageImageMultisample,
    &VkPhysicalDeviceFeatures::shaderStorageImageReadWithoutFormat,
    &VkPhysicalDeviceFeatures::shaderStorageImageWriteWithoutFormat,
    &VkPhysicalDeviceFeatures::shaderUniformBufferArrayDynamicIndexing,
    &VkPhysicalDeviceFeatures::shaderSampledImageArrayDynamicIndexing,
    &VkPhysicalDeviceFeatures::shaderStorageBufferArrayDynamicIndexing,
    &VkPhysicalDeviceFeatures::shaderStorageImageArrayDynamicIndexing,
    &VkPhysicalDeviceFeatures::shaderClipDistance,
    &VkPhysicalDeviceFeatures::shaderCullDistance,
    &VkPhysicalDeviceFeatures::shaderFloat64,
    &VkPhysicalDeviceFeatures::shaderInt64,
    &VkPhysicalDeviceFeatures::shaderInt16,
    &VkPhysicalDeviceFeatures::shaderResourceResidency,
    &VkPhysicalDeviceFeatures::shaderResourceMinLod,
    &VkPhysicalDeviceFeatures::sparseBinding,
    &VkPhysicalDeviceFeatures::sparseResidencyBuffer,
    &VkPhysicalDeviceFeatures::sparseResidencyImage2D,
    &VkPhysicalDeviceFeatures::sparseResidencyImage3D,
    &VkPhysicalDeviceFeatures::sparseResidency2Samples,
    &VkPhysicalDeviceFeatures::sparseResidency4Samples,
    &VkPhysicalDeviceFeatures::sparseResidency8Samples,
    &VkPhysicalDeviceFeatures::sparseResidency16Samples,
    &VkPhysicalDeviceFeatures::sparseResidencyAliased,
    &VkPhysicalDeviceFeatures::variableMultisampleRate,
    &VkPhysicalDeviceFeatures::inheritedQueries,
};

constexpr std::array core_1_1_members{
    &VkPhysicalDeviceVulkan11Features::storageBuffer16BitAccess,
    &VkPhysicalDeviceVulkan11Features::uniformAndStorageBuffer16BitAccess,
    &VkPhysicalDeviceVulkan11Features::storagePushConstant16,
    &VkPhysicalDeviceVulkan11Features::storageInputOutput16,
    &VkPhysicalDeviceVulkan11Features::multiview,
    &VkPhysicalDeviceVulkan11Features::multiviewGeometryShader,
    &VkPhysicalDeviceVulkan11Features::multiviewTessellationShader,
    &VkPhysicalDeviceVulkan11Features::variablePointersStorageBuffer,
    &VkPhysicalDeviceVulkan11Features::variablePointers,
    &VkPhysicalDeviceVulkan11Features::protectedMemory,
    &VkPhysicalDeviceVulkan11Features::samplerYcbcrConversion,
    &VkPhysicalDeviceVulkan11Features::shaderDrawParameters,
};

constexpr std::array core_1_2_members{
    &VkPhysicalDeviceVulkan12Features::samplerMirrorClampToEdge,
    &VkPhysicalDeviceVulkan12Features::drawIndirectCount,
    &VkPhysicalDeviceVulkan12Features::storageBuffer8BitAccess,
    &VkPhysicalDeviceVulkan12Features::uniformAndStorageBuffer8BitAccess,
    &VkPhysicalDeviceVulkan12Features::storagePushConstant8,
    &VkPhysicalDeviceVulkan12Features::shaderBufferInt64Atomics,
    &VkPhysicalDeviceVulkan12Features::shaderSharedInt64Atomics,
    &VkPhysicalDeviceVulkan12Features::shaderFloat16,
    &VkPhysicalDeviceVulkan12Features::shaderInt8,
    &VkPhysicalDeviceVulkan12Features::descriptorIndexing,
    &VkPhysicalDeviceVulkan12Features::shaderInputAttachmentArrayDynamicIndexing,
    &VkPhysicalDeviceVulkan12Features::shaderUniformTexelBufferArrayDynamicIndexing,
    &VkPhysicalDeviceVulkan12Features::shaderStorageTexelBufferArrayDynamicIndexing,
    &VkPhysicalDeviceVulkan12Features::shaderUniformBufferArrayNonUniformIndexing,
    &VkPhysicalDeviceVulkan12Features::shaderSampledImageArrayNonUniformIndexing,
    &VkPhysicalDeviceVulkan12Features::shaderStorageBufferArrayNonUniformIndexing,
    &VkPhysicalDeviceVulkan12Features::shaderStorageImageArrayNonUniformIndexing,
    &VkPhysicalDeviceVulkan12Features::shaderInputAttachmentArrayNonUniformIndexing,
    &VkPhysicalDeviceVulkan12Features::shaderUniformTexelBufferArrayNonUniformIndexing,
    &VkPhysicalDeviceVulkan12Features::shaderStorageTexelBufferArrayNonUniformIndexing,
    &VkPhysicalDeviceVulkan12Features::descriptorBindingUniformBufferUpdateAfterBind,
    &VkPhysicalDeviceVulkan12Features::descriptorBindingSampledImageUpdateAfterBind,
    &VkPhysicalDeviceVulkan12Features::descriptorBindingStorageImageUpdateAfterBind,
    &VkPhysicalDeviceVulkan12Features::descriptorBindingStorageBufferUpdateAfterBind,
    &VkPhysicalDeviceVulkan12Features::descriptorBindingUniformTexelBufferUpdateAfterBind,
    &VkPhysicalDeviceVulkan12Features::descriptorBindingStorageTexelBufferUpdateAfterBind,
    &VkPhysicalDeviceVulkan12Features::descriptorBindingUpdateUnusedWhilePending,
    &VkPhysicalDeviceVulkan12Features::descriptorBindingPartiallyBound,
    &VkPhysicalDeviceVulkan12Features::descriptorBindingVariableDescriptorCount,
    &VkPhysicalDeviceVulkan12Features::runtimeDescriptorArray,
    &VkPhysicalDeviceVulkan12Features::samplerFilterMinmax,
    &VkPhysicalDeviceVulkan12Features::scalarBlockLayout,
    &VkPhysicalDeviceVulkan12Features::imagelessFramebuffer,
    &VkPhysicalDeviceVulkan12Features::uniformBufferStandardLayout,
    &VkPhysicalDeviceVulkan12Features::shaderSubgroupExtendedTypes,
    &VkPhysicalDeviceVulkan12Features::separateDepthStencilLayouts,
    &VkPhysicalDeviceVulkan12Features::hostQueryReset,
    &VkPhysicalDeviceVulkan12Features::timelineSemaphore,
    &VkPhysicalDeviceVulkan12Features::bufferDeviceAddress,
    &VkPhysicalDeviceVulkan12Features::bufferDeviceAddressCaptureReplay,
    &VkPhysicalDeviceVulkan12Features::bufferDeviceAddressMultiDevice,
    &VkPhysicalDeviceVulkan12Features::vulkanMemoryModel,
    &VkPhysicalDeviceVulkan12Features::vulkanMemoryModelDeviceScope,
    &VkPhysicalDeviceVulkan12Features::vulkanMemoryModelAvailabilityVisibilityChains,
    &VkPhysicalDeviceVulkan12Features::shaderOutputViewportIndex,
    &VkPhysicalDeviceVulkan12Features::shaderOutputLayer,
    &VkPhysicalDeviceVulkan12Features::subgroupBroadcastDynamicId,
};

constexpr std::array core_1_3_members{
    &VkPhysicalDeviceVulkan13Features::robustImageAccess,
    &VkPhysicalDeviceVulkan13Features::inlineUniformBlock,
    &VkPhysicalDeviceVulkan13Features::descriptorBindingInlineUniformBlockUpdateAfterBind,
    &VkPhysicalDeviceVulkan13Features::pipelineCreationCacheControl,
    &VkPhysicalDeviceVulkan13Features::privateData,
    &VkPhysicalDeviceVulkan13Features::shaderDemoteToHelperInvocation,
    &VkPhysicalDeviceVulkan13Features::shaderTerminateInvocation,
    &VkPhysicalDeviceVulkan13Features::subgroupSizeControl,
    &VkPhysicalDeviceVulkan13Features::computeFullSubgroups,
    &VkPhysicalDeviceVulkan13Features::synchronization2,
    &VkPhysicalDeviceVulkan13Features::textureCompressionASTC_HDR,
    &VkPhysicalDeviceVulkan13Features::shaderZeroInitializeWorkgroupMemory,
    &VkPhysicalDeviceVulkan13Features::dynamicRendering,
    &VkPhysicalDeviceVulkan13Features::shaderIntegerDotProduct,
    &VkPhysicalDeviceVulkan13Features::maintenance4,
};

template<typename T, size_t N>
bool SupportsRequiredMembers(
    const T&                            _supported,
    const T&                            _required,
    const std::array<VkBool32 T::*, N>& _members
) {
    for (const auto member : _members) {
        if (_required.*member == VK_TRUE && _supported.*member == VK_FALSE) {
            return false;
        }
    }
    return true;
}

} // namespace

VulkanDeviceFeatures::VulkanDeviceFeatures(uint32 _api_version) noexcept : api_version(_api_version) {
    RebuildFeatureChain();
}

VulkanDeviceFeatures::VulkanDeviceFeatures(const VulkanDeviceFeatures& _other) noexcept :
    api_version(_other.api_version),
    core_1_0(_other.core_1_0),
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
        core_1_0    = _other.core_1_0;
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

VulkanDeviceFeatures VulkanDeviceFeatures::BuildRequiredFeatures(uint32 _api_version) {
    VulkanDeviceFeatures required_features(_api_version);

    // 1.0 features
    required_features.core_1_0.samplerAnisotropy = VK_TRUE;
    required_features.core_1_0.sampleRateShading = VK_TRUE;
    required_features.core_1_0.depthClamp        = VK_TRUE;
    // HLSL computes bindless index-table addresses with uint64_t arithmetic.
    required_features.core_1_0.shaderInt64       = VK_TRUE;
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

bool VulkanDeviceFeatures::SupportsAll(const VulkanDeviceFeatures& _required_features) const {
    return SupportsRequiredMembers(core_1_0, _required_features.core_1_0, core_1_0_members) &&
           SupportsRequiredMembers(core_1_1, _required_features.core_1_1, core_1_1_members) &&
           SupportsRequiredMembers(core_1_2, _required_features.core_1_2, core_1_2_members) &&
           SupportsRequiredMembers(core_1_3, _required_features.core_1_3, core_1_3_members);
}

VulkanDeviceFeatures
VulkanDeviceFeatures::QuerySupportedFeatures(VkPhysicalDevice _gpu, uint32 _api_version) {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(_gpu, &properties);
    const uint32              query_api_version = std::min(_api_version, properties.apiVersion);
    VulkanDeviceFeatures      features(query_api_version);
    VkPhysicalDeviceFeatures2 features2{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, features.RebuildFeatureChain()
    };
    if (query_api_version >= VK_API_VERSION_1_2) {
        vkGetPhysicalDeviceFeatures2(_gpu, &features2);
    } else if (query_api_version >= VK_API_VERSION_1_1) {
        vkGetPhysicalDeviceFeatures2(_gpu, &features2);
        features.core_1_1_nodes.WriteInto(features.core_1_1);
    } else {
        vkGetPhysicalDeviceFeatures(_gpu, &features.core_1_0);
        return features;
    }
    features.core_1_0 = features2.features;
    return features;
}

void* VulkanDeviceFeatures::RebuildFeatureChain() {
    core_1_1.sType             = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    core_1_2.sType             = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    core_1_3.sType             = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    core_1_1.pNext             = nullptr;
    core_1_2.pNext             = nullptr;
    core_1_3.pNext             = nullptr;
    core_1_1_nodes.tail->pNext = nullptr;

    void* chain_head = nullptr;
    if (api_version >= VK_API_VERSION_1_3) {
        PrependFeature(chain_head, core_1_3);
        PrependFeature(chain_head, core_1_2);
        PrependFeature(chain_head, core_1_1);
    } else if (api_version >= VK_API_VERSION_1_2) {
        PrependFeature(chain_head, core_1_2);
        PrependFeature(chain_head, core_1_1);
    } else if (api_version >= VK_API_VERSION_1_1) {
        // VkPhysicalDeviceVulkan11Features groups 1.1 capabilities but was introduced
        // in Vulkan 1.2. Use the individual feature structures for the 1.1 path.
        // https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceVulkan11Features.html
        PrependFeature(chain_head, core_1_1_nodes);
    } else {
        // Do nothing, only core 1.0 features are available.
    }
    return chain_head;
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

void VulkanDeviceFeatures::Vulkan11FeatureNodes::WriteInto(VkPhysicalDeviceVulkan11Features& _features
) const {
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
    void* chain_head                     = RebuildFeatureChain();
    _device_create_info.pEnabledFeatures = &core_1_0;
    if (api_version < VK_API_VERSION_1_1) {
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
    _device_create_info.pNext = chain_head;
}

} // namespace Moer::Render
