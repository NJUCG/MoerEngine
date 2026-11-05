#include "rhi/backends/vulkan/VulkanDeviceFeature.h"

#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace Moer::Render;

namespace {
void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

Moer::uint32 api_version = VK_API_VERSION_1_3;
VkPhysicalDeviceFeatures core10{};
VkPhysicalDeviceVulkan12Features core12{};
VkPhysicalDeviceVulkan13Features core13{};
Moer::uint32 query_count = 0;

VKAPI_ATTR void VKAPI_CALL GetProperties(VkPhysicalDevice, VkPhysicalDeviceProperties* out) {
    *out = {};
    out->apiVersion = api_version;
}

VKAPI_ATTR void VKAPI_CALL GetFeatures(VkPhysicalDevice, VkPhysicalDeviceFeatures2* out) {
    ++query_count;
    out->features = core10;
    auto* node11 = static_cast<VkPhysicalDeviceVulkan11Features*>(out->pNext);
    Check(node11 && node11->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, "missing 1.1 node");
    node11->multiview = VK_TRUE;
    auto* node12 = static_cast<VkPhysicalDeviceVulkan12Features*>(node11->pNext);
    Check(node12 && node12->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, "missing 1.2 node");
    auto* node13 = static_cast<VkPhysicalDeviceVulkan13Features*>(node12->pNext);
    Check(node13 && node13->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, "missing 1.3 node");
    Check(node13->pNext == nullptr, "core chain has an unexpected tail");
    auto* next = node12->pNext;
    *node12 = core12;
    node12->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    node12->pNext = next;
    *node13 = core13;
    node13->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
}

constexpr auto required10 = std::to_array<VkBool32 VkPhysicalDeviceFeatures::*>({
    &VkPhysicalDeviceFeatures::samplerAnisotropy, &VkPhysicalDeviceFeatures::sampleRateShading,
    &VkPhysicalDeviceFeatures::depthClamp, &VkPhysicalDeviceFeatures::shaderInt64
});
constexpr auto required12 = std::to_array<VkBool32 VkPhysicalDeviceVulkan12Features::*>({
    &VkPhysicalDeviceVulkan12Features::timelineSemaphore, &VkPhysicalDeviceVulkan12Features::bufferDeviceAddress,
    &VkPhysicalDeviceVulkan12Features::descriptorIndexing,
    &VkPhysicalDeviceVulkan12Features::shaderUniformBufferArrayNonUniformIndexing,
    &VkPhysicalDeviceVulkan12Features::descriptorBindingUniformBufferUpdateAfterBind,
    &VkPhysicalDeviceVulkan12Features::shaderSampledImageArrayNonUniformIndexing,
    &VkPhysicalDeviceVulkan12Features::descriptorBindingSampledImageUpdateAfterBind,
    &VkPhysicalDeviceVulkan12Features::shaderStorageImageArrayNonUniformIndexing,
    &VkPhysicalDeviceVulkan12Features::descriptorBindingStorageImageUpdateAfterBind,
    &VkPhysicalDeviceVulkan12Features::shaderStorageBufferArrayNonUniformIndexing,
    &VkPhysicalDeviceVulkan12Features::descriptorBindingStorageBufferUpdateAfterBind,
    &VkPhysicalDeviceVulkan12Features::drawIndirectCount
});
constexpr auto required13 = std::to_array<VkBool32 VkPhysicalDeviceVulkan13Features::*>({
    &VkPhysicalDeviceVulkan13Features::synchronization2, &VkPhysicalDeviceVulkan13Features::dynamicRendering
});

template<typename T, size_t N>
void CheckRequirements(T& values, const std::array<VkBool32 T::*, N>& members) {
    for (auto member : members) {
        values.*member = VK_FALSE;
        Check(!VulkanDeviceFeatures::QuerySupportedFeatures(VK_NULL_HANDLE).SupportsRequiredFeatures(), "missing feature accepted");
        values.*member = VK_TRUE;
    }
}

void CheckSnapshot(const VulkanDeviceFeatures& value) {
    Check(value.SupportsRequiredFeatures(), "required feature values lost");
    Check(value.GetCore11Features().multiview && value.GetCore10Features().tessellationShader, "optional feature values lost");
    Check(!value.GetCore11Features().pNext && !value.GetCore12Features().pNext && !value.GetCore13Features().pNext,
          "snapshot retained temporary native pointers");
}
}

int main() {
    const auto original_properties = vkGetPhysicalDeviceProperties;
    const auto original_features = vkGetPhysicalDeviceFeatures2;
    vkGetPhysicalDeviceProperties = GetProperties;
    vkGetPhysicalDeviceFeatures2 = GetFeatures;
    int result = 0;
    try {
        for (auto member : required10) core10.*member = VK_TRUE;
        for (auto member : required12) core12.*member = VK_TRUE;
        for (auto member : required13) core13.*member = VK_TRUE;
        core10.tessellationShader = VK_TRUE;
        CheckRequirements(core10, required10);
        CheckRequirements(core12, required12);
        CheckRequirements(core13, required13);
        for (auto version : {VK_API_VERSION_1_3, VK_API_VERSION_1_4}) {
            api_version = version;
            auto snapshot = VulkanDeviceFeatures::QuerySupportedFeatures(VK_NULL_HANDLE);
            auto copy = snapshot;
            std::vector<VulkanDeviceFeatures> relocated;
            relocated.push_back(std::move(copy));
            relocated.push_back(snapshot);
            for (const auto& value : relocated) CheckSnapshot(value);
        }
        for (auto version : {VK_API_VERSION_1_0, VK_API_VERSION_1_1, VK_API_VERSION_1_2}) {
            api_version = version;
            const auto before = query_count;
            bool rejected = false;
            try { (void)VulkanDeviceFeatures::QuerySupportedFeatures(VK_NULL_HANDLE); }
            catch (const std::runtime_error&) { rejected = true; }
            Check(rejected && query_count == before, "older device reached the 1.3 feature query");
        }
        std::cout << "Vulkan 1.3 feature requirements and detached snapshots passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    vkGetPhysicalDeviceProperties = original_properties;
    vkGetPhysicalDeviceFeatures2 = original_features;
    return result;
}
