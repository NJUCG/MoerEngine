#include "rhi/backends/vulkan/VulkanDeviceProperty.h"

#include <iostream>
#include <stdexcept>
#include <vector>

using namespace Moer::Render;

namespace {
Moer::uint32 api_version = VK_API_VERSION_1_3;
Moer::uint32 query_count = 0;

void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

VKAPI_ATTR void VKAPI_CALL GetProperties(VkPhysicalDevice, VkPhysicalDeviceProperties* out) {
    *out = {};
    out->apiVersion = api_version;
    out->limits.maxImageDimension2D = 8192;
}

VKAPI_ATTR void VKAPI_CALL GetProperties2(VkPhysicalDevice gpu, VkPhysicalDeviceProperties2* out) {
    ++query_count;
    GetProperties(gpu, &out->properties);
    auto* core11 = static_cast<VkPhysicalDeviceVulkan11Properties*>(out->pNext);
    Check(core11 && core11->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES, "missing 1.1 properties");
    core11->deviceUUID[0] = 17;
    core11->maxMultiviewViewCount = 6;
    auto* core12 = static_cast<VkPhysicalDeviceVulkan12Properties*>(core11->pNext);
    Check(core12 && core12->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES, "missing 1.2 properties");
    core12->maxUpdateAfterBindDescriptorsInAllPools = 4096;
    auto* core13 = static_cast<VkPhysicalDeviceVulkan13Properties*>(core12->pNext);
    Check(core13 && core13->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES, "missing 1.3 properties");
    core13->maxBufferSize = VkDeviceSize{1} << 35;
    Check(core13->pNext == nullptr, "property chain has an unexpected tail");
}

void CheckSnapshot(const VulkanCoreDeviceProperties& value) {
    Check(value.core_1_0.apiVersion == api_version && value.core_1_0.limits.maxImageDimension2D == 8192,
          "base properties lost");
    Check(value.core_1_1.deviceUUID[0] == 17 && value.core_1_1.maxMultiviewViewCount == 6,
          "1.1 properties lost");
    Check(value.core_1_2.maxUpdateAfterBindDescriptorsInAllPools == 4096 && value.core_1_3.maxBufferSize == VkDeviceSize{1} << 35,
          "1.2/1.3 properties lost");
    Check(!value.core_1_1.pNext && !value.core_1_2.pNext && !value.core_1_3.pNext,
          "snapshot retained temporary native pointers");
}
}

int main() {
    const auto original_properties = vkGetPhysicalDeviceProperties;
    const auto original_properties2 = vkGetPhysicalDeviceProperties2;
    vkGetPhysicalDeviceProperties = GetProperties;
    vkGetPhysicalDeviceProperties2 = GetProperties2;
    int result = 0;
    try {
        for (auto version : {VK_API_VERSION_1_3, VK_API_VERSION_1_4}) {
            api_version = version;
            auto value = VulkanCoreDeviceProperties::QueryGpuProperties(VK_NULL_HANDLE);
            auto assigned = value;
            std::vector<VulkanCoreDeviceProperties> relocated;
            relocated.push_back(std::move(value));
            relocated.push_back(assigned);
            for (const auto& snapshot : relocated) CheckSnapshot(snapshot);
        }
        for (auto version : {VK_API_VERSION_1_0, VK_API_VERSION_1_1, VK_API_VERSION_1_2}) {
            api_version = version;
            const auto before = query_count;
            bool rejected = false;
            try { (void)VulkanCoreDeviceProperties::QueryGpuProperties(VK_NULL_HANDLE); }
            catch (const std::runtime_error&) { rejected = true; }
            Check(rejected && before == query_count, "older device reached the 1.3 property query");
        }
        std::cout << "Vulkan 1.3 property query and detached snapshots passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    vkGetPhysicalDeviceProperties = original_properties;
    vkGetPhysicalDeviceProperties2 = original_properties2;
    return result;
}
