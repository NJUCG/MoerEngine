#include "rhi/vulkan/VulkanDeviceFeature.h"

#include <cstddef>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {

using Moer::Render::VulkanDeviceFeatures;

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
void CheckRequiredFeature(
    VkBool32 T::*                          _member,
    const T& (VulkanDeviceFeatures::*_getter)() const
) {
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
    _features.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    _features.pNext = &_features;
    const size_t feature_end = _last_member_offset + sizeof(VkBool32);
    auto* bytes = reinterpret_cast<unsigned char*>(&_features);
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

} // namespace

int main() {
    try {
        VulkanDeviceFeatures empty;
        Check(empty.SupportsAll(empty), "empty requirements were rejected");
        CheckRequiredFeature(&VkPhysicalDeviceFeatures::robustBufferAccess, &VulkanDeviceFeatures::GetCore10Features);
        CheckRequiredFeature(&VkPhysicalDeviceFeatures::inheritedQueries, &VulkanDeviceFeatures::GetCore10Features);
        CheckRequiredFeature(&VkPhysicalDeviceVulkan11Features::storageBuffer16BitAccess, &VulkanDeviceFeatures::GetCore11Features);
        CheckRequiredFeature(&VkPhysicalDeviceVulkan11Features::shaderDrawParameters, &VulkanDeviceFeatures::GetCore11Features);
        CheckRequiredFeature(&VkPhysicalDeviceVulkan12Features::timelineSemaphore, &VulkanDeviceFeatures::GetCore12Features);
        CheckRequiredFeature(&VkPhysicalDeviceVulkan12Features::subgroupBroadcastDynamicId, &VulkanDeviceFeatures::GetCore12Features);
        CheckRequiredFeature(&VkPhysicalDeviceVulkan13Features::synchronization2, &VulkanDeviceFeatures::GetCore13Features);
        CheckRequiredFeature(&VkPhysicalDeviceVulkan13Features::maintenance4, &VulkanDeviceFeatures::GetCore13Features);
        CheckComparisonIgnoresMetadataAndPadding();
        std::cout << "Vulkan device feature contracts passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Vulkan device feature contract failed: " << error.what() << '\n';
        return 1;
    }
}
