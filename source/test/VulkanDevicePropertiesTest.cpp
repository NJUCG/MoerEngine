#include "misc/Types.h"
#include "rhi/backends/vulkan/VulkanDeviceProperty.h"

#include <algorithm>
#include <iostream>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using Moer::Render::VulkanCoreDeviceProperties;

static_assert(std::is_trivially_copyable_v<VulkanCoreDeviceProperties>);

void Check(bool _condition, const char* _message) {
    if (!_condition) {
        throw std::runtime_error(_message);
    }
}

const VkPhysicalDeviceVulkan11Properties expected_core11 = [] {
    VkPhysicalDeviceVulkan11Properties properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES};
    for (Moer::uint32 index = 0; index < VK_UUID_SIZE; ++index) {
        properties.deviceUUID[index] = static_cast<Moer::uint8>(index + 1);
        properties.driverUUID[index] = static_cast<Moer::uint8>(index + 33);
    }
    for (Moer::uint32 index = 0; index < VK_LUID_SIZE; ++index) {
        properties.deviceLUID[index] = static_cast<Moer::uint8>(index + 65);
    }
    properties.deviceNodeMask              = 3;
    properties.deviceLUIDValid             = VK_TRUE;
    properties.subgroupSize                = 32;
    properties.subgroupSupportedStages     = VK_SHADER_STAGE_COMPUTE_BIT;
    properties.subgroupSupportedOperations = VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_VOTE_BIT;
    properties.subgroupQuadOperationsInAllStages = VK_TRUE;
    properties.pointClippingBehavior             = VK_POINT_CLIPPING_BEHAVIOR_USER_CLIP_PLANES_ONLY;
    properties.maxMultiviewViewCount             = 6;
    properties.maxMultiviewInstanceIndex         = 127;
    properties.protectedNoFault                  = VK_TRUE;
    properties.maxPerSetDescriptors              = 1024;
    properties.maxMemoryAllocationSize           = VkDeviceSize{1} << 34;
    return properties;
}();

Moer::uint32 physical_api_version = VK_API_VERSION_1_0;
Moer::uint32 query_api_version    = VK_API_VERSION_1_0;
Moer::uint32 base_query_count     = 0;
Moer::uint32 chain_query_count    = 0;

VKAPI_ATTR void VKAPI_CALL MockGetProperties(VkPhysicalDevice, VkPhysicalDeviceProperties* _properties) {
    ++base_query_count;
    *_properties                            = {};
    _properties->apiVersion                 = physical_api_version;
    _properties->limits.maxImageDimension2D = 8192;
}

VKAPI_ATTR void VKAPI_CALL MockGetProperties2(VkPhysicalDevice, VkPhysicalDeviceProperties2* _properties) {
    ++chain_query_count;
    Check(query_api_version >= VK_API_VERSION_1_1, "Vulkan 1.0 called the core Properties2 entry point");
    Check(_properties->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, "invalid query root");
    _properties->properties.apiVersion                 = physical_api_version;
    _properties->properties.limits.maxImageDimension2D = 8192;

    std::set<VkStructureType> expected_types;
    if (query_api_version < VK_API_VERSION_1_2) {
        expected_types = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES,
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES,
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_POINT_CLIPPING_PROPERTIES,
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES,
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_PROPERTIES,
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES
        };
    } else {
        expected_types = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES,
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES
        };
        if (query_api_version >= VK_API_VERSION_1_3) {
            expected_types.insert(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES);
        }
    }

    for (auto* node = static_cast<VkBaseOutStructure*>(_properties->pNext); node; node = node->pNext) {
        Check(expected_types.erase(node->sType) == 1, "unexpected, duplicate, or cyclic query node");
        switch (node->sType) {
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES: {
                auto& identity = *reinterpret_cast<VkPhysicalDeviceIDProperties*>(node);
                std::copy_n(expected_core11.deviceUUID, VK_UUID_SIZE, identity.deviceUUID);
                std::copy_n(expected_core11.driverUUID, VK_UUID_SIZE, identity.driverUUID);
                std::copy_n(expected_core11.deviceLUID, VK_LUID_SIZE, identity.deviceLUID);
                identity.deviceNodeMask  = expected_core11.deviceNodeMask;
                identity.deviceLUIDValid = expected_core11.deviceLUIDValid;
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES: {
                auto& subgroup               = *reinterpret_cast<VkPhysicalDeviceSubgroupProperties*>(node);
                subgroup.subgroupSize        = expected_core11.subgroupSize;
                subgroup.supportedStages     = expected_core11.subgroupSupportedStages;
                subgroup.supportedOperations = expected_core11.subgroupSupportedOperations;
                subgroup.quadOperationsInAllStages = expected_core11.subgroupQuadOperationsInAllStages;
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_POINT_CLIPPING_PROPERTIES:
                reinterpret_cast<VkPhysicalDevicePointClippingProperties*>(node)->pointClippingBehavior =
                    expected_core11.pointClippingBehavior;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES: {
                auto& multiview = *reinterpret_cast<VkPhysicalDeviceMultiviewProperties*>(node);
                multiview.maxMultiviewViewCount     = expected_core11.maxMultiviewViewCount;
                multiview.maxMultiviewInstanceIndex = expected_core11.maxMultiviewInstanceIndex;
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_PROPERTIES:
                reinterpret_cast<VkPhysicalDeviceProtectedMemoryProperties*>(node)->protectedNoFault =
                    expected_core11.protectedNoFault;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES: {
                auto& maintenance3 = *reinterpret_cast<VkPhysicalDeviceMaintenance3Properties*>(node);
                maintenance3.maxPerSetDescriptors    = expected_core11.maxPerSetDescriptors;
                maintenance3.maxMemoryAllocationSize = expected_core11.maxMemoryAllocationSize;
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES: {
                void* next       = node->pNext;
                auto& properties = *reinterpret_cast<VkPhysicalDeviceVulkan11Properties*>(node);
                properties       = expected_core11;
                properties.pNext = next;
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES:
                reinterpret_cast<VkPhysicalDeviceVulkan12Properties*>(node)
                    ->maxUpdateAfterBindDescriptorsInAllPools = 4096;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES:
                reinterpret_cast<VkPhysicalDeviceVulkan13Properties*>(node)->maxBufferSize = VkDeviceSize{1}
                                                                                             << 35;
                break;
            default:
                Check(false, "unhandled query node");
        }
    }
    Check(expected_types.empty(), "query omitted a required property structure");
}

void CheckSnapshot(const VulkanCoreDeviceProperties& _properties, Moer::uint32 _version) {
    Check(
        _properties.core_1_1.pNext == nullptr && _properties.core_1_2.pNext == nullptr &&
            _properties.core_1_3.pNext == nullptr,
        "property snapshot retained temporary chain pointers"
    );
    Check(
        _properties.core_1_1.sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES &&
            _properties.core_1_2.sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES &&
            _properties.core_1_3.sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES,
        "snapshot lost its native structure types"
    );

    const VkPhysicalDeviceVulkan11Properties empty{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES};
    const auto& expected = _version >= VK_API_VERSION_1_1 ? expected_core11 : empty;
    const auto& actual   = _properties.core_1_1;
    Check(
        std::equal(actual.deviceUUID, actual.deviceUUID + VK_UUID_SIZE, expected.deviceUUID),
        "device UUID lost"
    );
    Check(
        std::equal(actual.driverUUID, actual.driverUUID + VK_UUID_SIZE, expected.driverUUID),
        "driver UUID lost"
    );
    Check(
        std::equal(actual.deviceLUID, actual.deviceLUID + VK_LUID_SIZE, expected.deviceLUID),
        "device LUID lost"
    );
    Check(
        actual.deviceNodeMask == expected.deviceNodeMask &&
            actual.deviceLUIDValid == expected.deviceLUIDValid,
        "device identity values lost"
    );
    Check(
        actual.subgroupSize == expected.subgroupSize &&
            actual.subgroupSupportedStages == expected.subgroupSupportedStages &&
            actual.subgroupSupportedOperations == expected.subgroupSupportedOperations &&
            actual.subgroupQuadOperationsInAllStages == expected.subgroupQuadOperationsInAllStages,
        "subgroup values lost"
    );
    Check(actual.pointClippingBehavior == expected.pointClippingBehavior, "point clipping value lost");
    Check(
        actual.maxMultiviewViewCount == expected.maxMultiviewViewCount &&
            actual.maxMultiviewInstanceIndex == expected.maxMultiviewInstanceIndex,
        "multiview values lost"
    );
    Check(actual.protectedNoFault == expected.protectedNoFault, "protected memory value lost");
    Check(
        actual.maxPerSetDescriptors == expected.maxPerSetDescriptors &&
            actual.maxMemoryAllocationSize == expected.maxMemoryAllocationSize,
        "maintenance3 values lost"
    );
    Check(
        _properties.core_1_2.maxUpdateAfterBindDescriptorsInAllPools ==
            (_version >= VK_API_VERSION_1_2 ? 4096u : 0u),
        "incorrect Vulkan 1.2 property values"
    );
    Check(
        _properties.core_1_3.maxBufferSize == (_version >= VK_API_VERSION_1_3 ? VkDeviceSize{1} << 35 : 0),
        "incorrect Vulkan 1.3 property values"
    );
}

void CheckQueryAndCopies(Moer::uint32 _requested_version, Moer::uint32 _physical_version) {
    physical_api_version = _physical_version;
    query_api_version    = std::min(_requested_version, _physical_version);
    base_query_count     = 0;
    chain_query_count    = 0;

    VulkanCoreDeviceProperties              assigned;
    std::vector<VulkanCoreDeviceProperties> relocated;
    relocated.reserve(1);
    {
        const auto queried =
            VulkanCoreDeviceProperties::QueryGpuProperties(VK_NULL_HANDLE, _requested_version);
        CheckSnapshot(queried, query_api_version);
        Check(
            queried.core_1_0.apiVersion == _physical_version &&
                queried.core_1_0.limits.maxImageDimension2D == 8192,
            "base properties were lost or API version was overwritten"
        );
        Check(
            base_query_count == 1 && chain_query_count == (query_api_version >= VK_API_VERSION_1_1 ? 1u : 0u),
            "incorrect query entry points or call counts"
        );
        VulkanCoreDeviceProperties copied = queried;
        assigned                          = copied;
        relocated.push_back(std::move(copied));
        relocated.push_back(queried); // Force relocation before the original query result goes out of scope.
    }
    CheckSnapshot(assigned, query_api_version);
    for (const auto& properties : relocated) {
        CheckSnapshot(properties, query_api_version);
    }
}

} // namespace

int main() {
    const auto original_get_properties  = vkGetPhysicalDeviceProperties;
    const auto original_get_properties2 = vkGetPhysicalDeviceProperties2;
    vkGetPhysicalDeviceProperties       = MockGetProperties;
    vkGetPhysicalDeviceProperties2      = MockGetProperties2;
    int result                          = 0;
    try {
        CheckSnapshot(VulkanCoreDeviceProperties{}, VK_API_VERSION_1_0);
        const Moer::uint32 versions[] = {
            VK_API_VERSION_1_0,
            VK_MAKE_API_VERSION(0, 1, 1, 7),
            VK_MAKE_API_VERSION(0, 1, 2, 13),
            VK_MAKE_API_VERSION(0, 1, 3, 22),
            VK_MAKE_API_VERSION(0, 1, 4, 1)
        };
        for (Moer::uint32 requested : versions) {
            for (Moer::uint32 physical : versions) {
                CheckQueryAndCopies(requested, physical);
            }
        }
        std::cout << "Vulkan device property query contracts passed (25 version combinations).\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    vkGetPhysicalDeviceProperties  = original_get_properties;
    vkGetPhysicalDeviceProperties2 = original_get_properties2;
    return result;
}
