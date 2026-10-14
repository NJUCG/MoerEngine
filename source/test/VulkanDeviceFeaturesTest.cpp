#include "rhi/vulkan/VulkanDeviceFeature.h"
#include "rhi/vulkan/VulkanDeviceFeatureChain.h"

#include <cstddef>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <type_traits>

namespace {

using Moer::Render::VulkanDeviceFeatureChain;
using Moer::Render::VulkanDeviceFeatures;

static_assert(std::is_copy_constructible_v<VulkanDeviceFeatures>);
static_assert(std::is_move_constructible_v<VulkanDeviceFeatures>);
static_assert(!std::is_copy_constructible_v<VulkanDeviceFeatureChain>);
static_assert(!std::is_move_constructible_v<VulkanDeviceFeatureChain>);

void Check(bool _condition, const char* _message) {
    if (!_condition) {
        throw std::runtime_error(_message);
    }
}

// The object is non-const; only its observation API is read-only. Tests supply
// native feature results without depending on a GPU or the platform backend.
template<typename T>
T& NativeFeatures(VulkanDeviceFeatures& _features, const T& (VulkanDeviceFeatures::*_getter)() const) {
    return const_cast<T&>((_features.*_getter)());
}

template<typename T>
void CheckRequiredFeature(VkBool32 T::* _member, const T& (VulkanDeviceFeatures::*_getter)() const) {
    VulkanDeviceFeatures supported;
    VulkanDeviceFeatures required;
    NativeFeatures(required, _getter).*_member = VK_TRUE;
    Check(!supported.SupportsAll(required), "missing required feature was accepted");
    NativeFeatures(supported, _getter).*_member = VK_TRUE;
    Check(supported.SupportsAll(required), "supported required feature was rejected");
    NativeFeatures(required, _getter).*_member = VK_FALSE;
    Check(supported.SupportsAll(required), "additional supported feature was rejected");
}

template<typename T>
void ChangeMetadataAndPadding(T& _features, size_t _last_member_offset) {
    _features.sType          = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    _features.pNext          = &_features;
    const size_t feature_end = _last_member_offset + sizeof(VkBool32);
    auto*        bytes       = reinterpret_cast<unsigned char*>(&_features);
    std::memset(bytes + feature_end, 0xa5, sizeof(T) - feature_end);
}

void CheckComparisonIgnoresMetadataAndPadding() {
    VulkanDeviceFeatures supported;
    VulkanDeviceFeatures required;
    ChangeMetadataAndPadding(
        NativeFeatures(required, &VulkanDeviceFeatures::GetCore11Features),
        offsetof(VkPhysicalDeviceVulkan11Features, shaderDrawParameters)
    );
    ChangeMetadataAndPadding(
        NativeFeatures(required, &VulkanDeviceFeatures::GetCore12Features),
        offsetof(VkPhysicalDeviceVulkan12Features, subgroupBroadcastDynamicId)
    );
    ChangeMetadataAndPadding(
        NativeFeatures(required, &VulkanDeviceFeatures::GetCore13Features),
        offsetof(VkPhysicalDeviceVulkan13Features, maintenance4)
    );
    Check(supported.SupportsAll(required), "metadata or padding affected feature support");
}

template<typename T>
T& ChainNode(void* _node, VkStructureType _expected_type) {
    Check(_node != nullptr, "missing native feature chain node");
    auto& node = *static_cast<T*>(_node);
    Check(node.sType == _expected_type, "unexpected native feature chain node");
    return node;
}

void CheckSnapshotPointers(const VulkanDeviceFeatures& _features) {
    Check(_features.GetCore11Features().pNext == nullptr, "snapshot retained a 1.1 chain pointer");
    Check(_features.GetCore12Features().pNext == nullptr, "snapshot retained a 1.2 chain pointer");
    Check(_features.GetCore13Features().pNext == nullptr, "snapshot retained a 1.3 chain pointer");
}

void CheckVulkan10Chain() {
    VulkanDeviceFeatures source;
    NativeFeatures(source, &VulkanDeviceFeatures::GetCore10Features).samplerAnisotropy = VK_TRUE;
    VulkanDeviceFeatureChain chain(source, VK_API_VERSION_1_0);
    Check(chain.GetFeatures2().pNext == nullptr, "1.0 used a features2 extension chain");
    VkDeviceCreateInfo                          create_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    VkPhysicalDeviceDescriptorBufferFeaturesEXT extension{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT
    };
    create_info.pNext = &extension;
    chain.AttachToDeviceCreateInfo(create_info);
    Check(create_info.pNext == &extension, "1.0 discarded the existing extension chain");
    Check(create_info.pEnabledFeatures == &chain.GetFeatures2().features, "1.0 did not use pEnabledFeatures");
    Check(
        chain.CaptureFeatures().GetCore10Features().samplerAnisotropy == VK_TRUE, "1.0 feature value was lost"
    );
}

void CheckVulkan11Chain() {
    VulkanDeviceFeatures source;
    auto&                features     = NativeFeatures(source, &VulkanDeviceFeatures::GetCore11Features);
    features.storageBuffer16BitAccess = VK_TRUE;
    features.uniformAndStorageBuffer16BitAccess = VK_TRUE;
    features.storagePushConstant16              = VK_TRUE;
    features.storageInputOutput16               = VK_TRUE;
    features.multiview                          = VK_TRUE;
    features.multiviewGeometryShader            = VK_TRUE;
    features.multiviewTessellationShader        = VK_TRUE;
    features.variablePointersStorageBuffer      = VK_TRUE;
    features.variablePointers                   = VK_TRUE;
    features.protectedMemory                    = VK_TRUE;
    features.samplerYcbcrConversion             = VK_TRUE;
    features.shaderDrawParameters               = VK_TRUE;

    VulkanDeviceFeatures snapshot;
    {
        VulkanDeviceFeatureChain chain(source, VK_MAKE_API_VERSION(0, 1, 1, 7));
        auto&                    storage = ChainNode<VkPhysicalDevice16BitStorageFeatures>(
            chain.GetFeatures2().pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES
        );
        auto& multiview = ChainNode<VkPhysicalDeviceMultiviewFeatures>(
            storage.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES
        );
        auto& pointers = ChainNode<VkPhysicalDeviceVariablePointersFeatures>(
            multiview.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES
        );
        auto& protected_memory = ChainNode<VkPhysicalDeviceProtectedMemoryFeatures>(
            pointers.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES
        );
        auto& ycbcr = ChainNode<VkPhysicalDeviceSamplerYcbcrConversionFeatures>(
            protected_memory.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES
        );
        auto& draw_parameters = ChainNode<VkPhysicalDeviceShaderDrawParametersFeatures>(
            ycbcr.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES
        );
        Check(draw_parameters.pNext == nullptr, "1.1 chain was not terminated");
        Check(
            storage.storageInputOutput16 && multiview.multiviewTessellationShader &&
                pointers.variablePointers && protected_memory.protectedMemory &&
                ycbcr.samplerYcbcrConversion && draw_parameters.shaderDrawParameters,
            "1.1 native feature mapping lost a value"
        );

        VkPhysicalDeviceDescriptorBufferFeaturesEXT extension{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT
        };
        VkDeviceCreateInfo create_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        create_info.pNext = &extension;
        chain.AttachToDeviceCreateInfo(create_info);
        Check(create_info.pNext == &chain.GetFeatures2(), "1.1 root was not attached");
        Check(create_info.pEnabledFeatures == nullptr, "features2 was combined with pEnabledFeatures");
        Check(draw_parameters.pNext == &extension, "1.1 discarded the extension tail");
        snapshot = chain.CaptureFeatures();
    }
    Check(
        snapshot.SupportsAll(source) && source.SupportsAll(snapshot), "1.1 round trip changed feature values"
    );
    Check(
        snapshot.GetCore12Features().timelineSemaphore == VK_FALSE, "1.1 captured an unavailable 1.2 feature"
    );
    CheckSnapshotPointers(snapshot);
}

void CheckModernChain(uint32_t _api_version, bool _has_vulkan13) {
    VulkanDeviceFeatures source;
    NativeFeatures(source, &VulkanDeviceFeatures::GetCore10Features).robustBufferAccess = VK_TRUE;
    NativeFeatures(source, &VulkanDeviceFeatures::GetCore11Features).multiview          = VK_TRUE;
    NativeFeatures(source, &VulkanDeviceFeatures::GetCore12Features).timelineSemaphore  = VK_TRUE;
    NativeFeatures(source, &VulkanDeviceFeatures::GetCore13Features).dynamicRendering   = VK_TRUE;

    VulkanDeviceFeatures snapshot;
    {
        VulkanDeviceFeatureChain chain(source, _api_version);
        auto&                    core11 = ChainNode<VkPhysicalDeviceVulkan11Features>(
            chain.GetFeatures2().pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES
        );
        auto& core12 = ChainNode<VkPhysicalDeviceVulkan12Features>(
            core11.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES
        );
        Check(core11.multiview && core12.timelineSemaphore, "modern native chain lost feature values");
        void** tail = &core12.pNext;
        if (_has_vulkan13) {
            auto& core13 = ChainNode<VkPhysicalDeviceVulkan13Features>(
                core12.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES
            );
            Check(core13.dynamicRendering == VK_TRUE, "1.3 native chain lost dynamicRendering");
            // Simulate a driver query writing a feature into the owned chain.
            core13.synchronization2 = VK_TRUE;
            tail                    = &core13.pNext;
        }
        Check(*tail == nullptr, "modern chain was not terminated");
        VkPhysicalDeviceDescriptorBufferFeaturesEXT extension{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT
        };
        VkDeviceCreateInfo create_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        create_info.pNext            = &extension;
        create_info.pEnabledFeatures = &source.GetCore10Features();
        chain.AttachToDeviceCreateInfo(create_info);
        Check(create_info.pNext == &chain.GetFeatures2(), "modern root was not attached");
        Check(create_info.pEnabledFeatures == nullptr, "modern chain retained pEnabledFeatures");
        Check(*tail == &extension, "modern chain discarded the extension tail");
        snapshot = chain.CaptureFeatures();
        CheckSnapshotPointers(source);
    }
    Check(
        snapshot.GetCore10Features().robustBufferAccess && snapshot.GetCore11Features().multiview &&
            snapshot.GetCore12Features().timelineSemaphore,
        "snapshot lost a feature after chain destruction"
    );
    Check(
        (snapshot.GetCore13Features().synchronization2 == VK_TRUE) == _has_vulkan13,
        "snapshot captured the wrong core version"
    );
    CheckSnapshotPointers(snapshot);
    const VulkanDeviceFeatures copy = snapshot;
    VulkanDeviceFeatures       assigned;
    assigned = copy;
    Check(assigned.SupportsAll(snapshot), "snapshot copy changed feature values");
    CheckSnapshotPointers(copy);
    CheckSnapshotPointers(assigned);
}

uint32_t mock_gpu_api_version   = VK_API_VERSION_1_3;
uint32_t mock_query_api_version = VK_API_VERSION_1_3;
uint32_t legacy_query_count     = 0;
uint32_t extended_query_count   = 0;

VKAPI_ATTR void VKAPI_CALL MockGetProperties(VkPhysicalDevice, VkPhysicalDeviceProperties* _properties) {
    *_properties            = {};
    _properties->apiVersion = mock_gpu_api_version;
}

VKAPI_ATTR void VKAPI_CALL MockGetFeatures(VkPhysicalDevice, VkPhysicalDeviceFeatures* _features) {
    ++legacy_query_count;
    *_features                   = {};
    _features->samplerAnisotropy = VK_TRUE;
}

VKAPI_ATTR void VKAPI_CALL MockGetFeatures2(VkPhysicalDevice, VkPhysicalDeviceFeatures2* _features) {
    ++extended_query_count;
    Check(_features->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, "query root had the wrong sType");
    _features->features.samplerAnisotropy = VK_TRUE;
    if (mock_query_api_version >= VK_API_VERSION_1_2) {
        auto& core11 = ChainNode<VkPhysicalDeviceVulkan11Features>(
            _features->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES
        );
        auto& core12 = ChainNode<VkPhysicalDeviceVulkan12Features>(
            core11.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES
        );
        core11.multiview         = VK_TRUE;
        core12.timelineSemaphore = VK_TRUE;
        if (mock_query_api_version >= VK_API_VERSION_1_3) {
            auto& core13 = ChainNode<VkPhysicalDeviceVulkan13Features>(
                core12.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES
            );
            core13.synchronization2 = VK_TRUE;
            Check(core13.pNext == nullptr, "query exceeded the available core version");
        } else {
            Check(core12.pNext == nullptr, "query requested 1.3 features from a 1.2 GPU");
        }
    } else {
        auto& storage = ChainNode<VkPhysicalDevice16BitStorageFeatures>(
            _features->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES
        );
        auto& multiview = ChainNode<VkPhysicalDeviceMultiviewFeatures>(
            storage.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES
        );
        multiview.multiview = VK_TRUE;
    }
}

class MockQueryFunctions {
public:
    MockQueryFunctions() {
        vkGetPhysicalDeviceProperties = MockGetProperties;
        vkGetPhysicalDeviceFeatures   = MockGetFeatures;
        vkGetPhysicalDeviceFeatures2  = MockGetFeatures2;
    }

    ~MockQueryFunctions() {
        vkGetPhysicalDeviceProperties = properties;
        vkGetPhysicalDeviceFeatures   = features;
        vkGetPhysicalDeviceFeatures2  = features2;
    }

private:
    PFN_vkGetPhysicalDeviceProperties properties = vkGetPhysicalDeviceProperties;
    PFN_vkGetPhysicalDeviceFeatures   features   = vkGetPhysicalDeviceFeatures;
    PFN_vkGetPhysicalDeviceFeatures2  features2  = vkGetPhysicalDeviceFeatures2;
};

void CheckSupportedQuery(uint32_t _requested_version, uint32_t _gpu_version, uint32_t _expected_version) {
    MockQueryFunctions mock;
    mock_gpu_api_version   = _gpu_version;
    mock_query_api_version = _expected_version;
    legacy_query_count     = 0;
    extended_query_count   = 0;
    const auto snapshot    = VulkanDeviceFeatures::QuerySupportedFeatures(VK_NULL_HANDLE, _requested_version);
    Check(snapshot.GetCore10Features().samplerAnisotropy == VK_TRUE, "query lost a base feature");
    Check(
        (snapshot.GetCore11Features().multiview == VK_TRUE) == (_expected_version >= VK_API_VERSION_1_1),
        "query captured the wrong 1.1 capabilities"
    );
    Check(
        (snapshot.GetCore12Features().timelineSemaphore == VK_TRUE) ==
            (_expected_version >= VK_API_VERSION_1_2),
        "query captured the wrong 1.2 capabilities"
    );
    Check(
        (snapshot.GetCore13Features().synchronization2 == VK_TRUE) ==
            (_expected_version >= VK_API_VERSION_1_3),
        "query captured the wrong 1.3 capabilities"
    );
    Check(
        legacy_query_count == (_expected_version < VK_API_VERSION_1_1 ? 1u : 0u),
        "wrong legacy query entry point count"
    );
    Check(
        extended_query_count == (_expected_version >= VK_API_VERSION_1_1 ? 1u : 0u),
        "wrong extended query entry point count"
    );
    CheckSnapshotPointers(snapshot);
}

void CheckRequiredFeatures() {
    const auto  required = VulkanDeviceFeatures::BuildRequiredFeatures(VK_API_VERSION_1_3);
    const auto& base     = required.GetCore10Features();
    Check(base.samplerAnisotropy && base.sampleRateShading && base.depthClamp, "base requirements changed");
    Check(
        base.sparseBinding == VK_FALSE && base.sparseResidencyBuffer == VK_FALSE,
        "unused sparse feature became required"
    );
    Check(
        required.GetCore12Features().timelineSemaphore && required.GetCore12Features().bufferDeviceAddress &&
            required.GetCore12Features().descriptorIndexing,
        "1.2 requirements changed"
    );
    Check(
        required.GetCore13Features().synchronization2 && required.GetCore13Features().dynamicRendering,
        "1.3 requirements changed"
    );
    CheckSnapshotPointers(required);
}

} // namespace

int main() {
    try {
        VulkanDeviceFeatures empty;
        Check(empty.SupportsAll(empty), "empty requirements were rejected");
        CheckRequiredFeature(
            &VkPhysicalDeviceFeatures::robustBufferAccess, &VulkanDeviceFeatures::GetCore10Features
        );
        CheckRequiredFeature(
            &VkPhysicalDeviceFeatures::inheritedQueries, &VulkanDeviceFeatures::GetCore10Features
        );
        CheckRequiredFeature(
            &VkPhysicalDeviceVulkan11Features::storageBuffer16BitAccess,
            &VulkanDeviceFeatures::GetCore11Features
        );
        CheckRequiredFeature(
            &VkPhysicalDeviceVulkan11Features::shaderDrawParameters, &VulkanDeviceFeatures::GetCore11Features
        );
        CheckRequiredFeature(
            &VkPhysicalDeviceVulkan12Features::timelineSemaphore, &VulkanDeviceFeatures::GetCore12Features
        );
        CheckRequiredFeature(
            &VkPhysicalDeviceVulkan12Features::subgroupBroadcastDynamicId,
            &VulkanDeviceFeatures::GetCore12Features
        );
        CheckRequiredFeature(
            &VkPhysicalDeviceVulkan13Features::synchronization2, &VulkanDeviceFeatures::GetCore13Features
        );
        CheckRequiredFeature(
            &VkPhysicalDeviceVulkan13Features::maintenance4, &VulkanDeviceFeatures::GetCore13Features
        );
        CheckComparisonIgnoresMetadataAndPadding();
        CheckVulkan10Chain();
        CheckVulkan11Chain();
        CheckModernChain(VK_API_VERSION_1_2, false);
        CheckModernChain(VK_MAKE_API_VERSION(0, 1, 2, 42), false);
        CheckModernChain(VK_API_VERSION_1_3, true);
        CheckModernChain(VK_MAKE_API_VERSION(0, 1, 3, 9), true);
        CheckRequiredFeatures();
        CheckSupportedQuery(VK_API_VERSION_1_0, VK_API_VERSION_1_3, VK_API_VERSION_1_0);
        CheckSupportedQuery(VK_API_VERSION_1_1, VK_API_VERSION_1_3, VK_API_VERSION_1_1);
        CheckSupportedQuery(
            VK_MAKE_API_VERSION(0, 1, 2, 42), VK_API_VERSION_1_3, VK_MAKE_API_VERSION(0, 1, 2, 42)
        );
        CheckSupportedQuery(VK_API_VERSION_1_3, VK_API_VERSION_1_3, VK_API_VERSION_1_3);
        CheckSupportedQuery(VK_API_VERSION_1_3, VK_API_VERSION_1_2, VK_API_VERSION_1_2);
        CheckSupportedQuery(VK_API_VERSION_1_3, VK_API_VERSION_1_0, VK_API_VERSION_1_0);
        std::cout << "Vulkan device feature contracts passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Vulkan device feature contract failed: " << error.what() << '\n';
        return 1;
    }
}
