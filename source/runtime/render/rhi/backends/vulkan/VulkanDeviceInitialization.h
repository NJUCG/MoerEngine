#pragma once

#include "VulkanDeviceFeature.h"
#include "VulkanDeviceProperty.h"
#include "VulkanTypeDefs.h"

#include <optional>
#include <span>

namespace Moer::Render {

struct QueueFamilyIndices {
    std::optional<uint32> graphics;
    std::optional<uint32> present;
    std::optional<uint32> transfer;
    std::optional<uint32> compute;

    bool IsComplete() const {
        return graphics.has_value() && present.has_value() && transfer.has_value() && compute.has_value();
    }

};

struct VulkanDeviceInfo {
    Array<const char*>               enabled_extensions{};
    VulkanDeviceCapabilities         capabilities{};
    VulkanDeviceFeatures             core_features{};
    VulkanCoreDeviceProperties       core_properties{};
    VulkanOptionalDeviceProperties   optional_properties{};
    VkPhysicalDeviceMemoryProperties memory_properties{};
    TQueueFamilyPropertiesArray      queue_family_props{};
    QueueFamilyIndices               queue_family_indices{};
};

namespace VulkanInitialization {
std::span<const char* const> RequiredInstanceExtensions();
std::span<const char* const> RequiredDeviceExtensions();

// Query optional capabilities and create the device while all native feature
// nodes are local. info retains only enabled names and detached value snapshots.
VkResult CreateDevice(VkPhysicalDevice gpu, VkDeviceCreateInfo request, VulkanDeviceInfo& info, VkDevice& device);
} // namespace VulkanInitialization

} // namespace Moer::Render
