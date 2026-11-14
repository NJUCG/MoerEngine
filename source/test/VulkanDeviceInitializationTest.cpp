#include "rhi/backends/vulkan/VulkanDeviceInitialization.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

using namespace Moer::Render;

namespace {
std::set<std::string>     advertised;
std::set<std::string>     enabled;
std::set<VkStructureType> queried;
std::set<VkStructureType> created;
bool                      maintenance5_available = true;
bool                      buffer_available       = true;
bool                      enum_incomplete        = false;
Moer::uint32              create_calls           = 0;
VkResult                  create_result          = VK_SUCCESS;

void Check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

VKAPI_ATTR VkResult VKAPI_CALL Enumerate(VkPhysicalDevice, const char*, Moer::uint32* count, VkExtensionProperties* out) {
    if (!out) {
        *count = static_cast<Moer::uint32>(advertised.size());
        return VK_SUCCESS;
    }
    const auto capacity = *count;
    *count              = 0;
    for (const auto& name : advertised) {
        if (*count == capacity)
            return VK_INCOMPLETE;
        out[*count] = {};
        std::memcpy(out[*count].extensionName, name.c_str(), name.size() + 1);
        ++*count;
    }
    if (enum_incomplete) {
        enum_incomplete = false;
        return VK_INCOMPLETE;
    }
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL Features(VkPhysicalDevice, VkPhysicalDeviceFeatures2* out) {
    for (auto* node = static_cast<VkBaseOutStructure*>(out->pNext); node; node = node->pNext) {
        queried.insert(node->sType);
        switch (node->sType) {
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR:
                reinterpret_cast<VkPhysicalDeviceMaintenance5FeaturesKHR*>(node)->maintenance5 = maintenance5_available;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT: {
                auto* feature                        = reinterpret_cast<VkPhysicalDeviceDescriptorHeapFeaturesEXT*>(node);
                feature->descriptorHeap              = VK_TRUE;
                feature->descriptorHeapCaptureReplay = VK_TRUE;
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT: {
                auto* feature                            = reinterpret_cast<VkPhysicalDeviceDescriptorBufferFeaturesEXT*>(node);
                feature->descriptorBuffer                = buffer_available;
                feature->descriptorBufferPushDescriptors = VK_TRUE;
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PRIORITY_FEATURES_EXT:
                reinterpret_cast<VkPhysicalDeviceMemoryPriorityFeaturesEXT*>(node)->memoryPriority = VK_TRUE;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PAGEABLE_DEVICE_LOCAL_MEMORY_FEATURES_EXT:
                reinterpret_cast<VkPhysicalDevicePageableDeviceLocalMemoryFeaturesEXT*>(node)->pageableDeviceLocalMemory = VK_TRUE;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR:
                reinterpret_cast<VkPhysicalDeviceCooperativeMatrixFeaturesKHR*>(node)->cooperativeMatrix = VK_TRUE;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_VECTOR_FEATURES_NV:
                reinterpret_cast<VkPhysicalDeviceCooperativeVectorFeaturesNV*>(node)->cooperativeVector = VK_TRUE;
                break;
            default:
                Check(false, "unexpected optional feature query");
        }
    }
}

VKAPI_ATTR void VKAPI_CALL Properties(VkPhysicalDevice, VkPhysicalDeviceProperties2* out) {
    for (auto* node = static_cast<VkBaseOutStructure*>(out->pNext); node; node = node->pNext) {
        if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT)
            reinterpret_cast<VkPhysicalDeviceDescriptorHeapPropertiesEXT*>(node)->maxPushDataSize = 256;
        if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_PROPERTIES_EXT)
            reinterpret_cast<VkPhysicalDeviceDescriptorBufferPropertiesEXT*>(node)->descriptorBufferOffsetAlignment = 64;
    }
}

VKAPI_ATTR VkResult VKAPI_CALL MatrixModes(VkPhysicalDevice, Moer::uint32* count, VkCooperativeMatrixPropertiesKHR* out) {
    if (!out) {
        *count = 2;
        return VK_SUCCESS;
    }
    Check(out[0].sType == VK_STRUCTURE_TYPE_COOPERATIVE_MATRIX_PROPERTIES_KHR && out[1].sType == VK_STRUCTURE_TYPE_COOPERATIVE_MATRIX_PROPERTIES_KHR, "matrix modes were not initialized");
    out[0].MSize = 16;
    *count       = 1;
    return VK_INCOMPLETE;
}

VKAPI_ATTR VkResult VKAPI_CALL VectorModes(VkPhysicalDevice, Moer::uint32*, VkCooperativeVectorPropertiesNV*) {
    return VK_ERROR_INITIALIZATION_FAILED;
}

VKAPI_ATTR VkResult VKAPI_CALL Create(VkPhysicalDevice, const VkDeviceCreateInfo* request, const VkAllocationCallbacks*, VkDevice* device) {
    ++create_calls;
    enabled.clear();
    created.clear();
    for (Moer::uint32 i = 0; i < request->enabledExtensionCount; ++i) {
        const std::string name(request->ppEnabledExtensionNames[i]);
        Check(advertised.contains(name) && enabled.insert(name).second, "invalid enabled extension list");
    }
    for (auto* node = static_cast<const VkBaseInStructure*>(request->pNext); node; node = node->pNext) {
        Check(created.insert(node->sType).second, "duplicate or cyclic create chain");
        if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT)
            Check(!reinterpret_cast<const VkPhysicalDeviceDescriptorHeapFeaturesEXT*>(node)->descriptorHeapCaptureReplay, "heap capture replay was enabled");
        if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT)
            Check(reinterpret_cast<const VkPhysicalDeviceDescriptorBufferFeaturesEXT*>(node)->descriptorBufferPushDescriptors == enabled.contains(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME), "push feature enabled without its extension");
    }
    Check(created.contains(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES) && created.contains(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) && created.contains(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES), "missing core feature chain");
    Check(request->pEnabledFeatures != nullptr, "missing core 1.0 features");
    if (create_result == VK_SUCCESS)
        *device = reinterpret_cast<VkDevice>(1);
    return create_result;
}

void Reset() {
    advertised.clear();
    for (const char* required : VulkanInitialization::RequiredDeviceExtensions())
        advertised.insert(required);
    maintenance5_available = true;
    buffer_available       = true;
    create_result          = VK_SUCCESS;
    queried.clear();
}

VkResult Initialize(VulkanDeviceInfo& info) {
    VkDevice           device = VK_NULL_HANDLE;
    VkDeviceCreateInfo request{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    const auto         result = VulkanInitialization::CreateDevice(VK_NULL_HANDLE, request, info, device);
    Check(!info.core_features.GetCore11Features().pNext && !info.core_features.GetCore12Features().pNext && !info.core_features.GetCore13Features().pNext, "create mutated the core snapshot");
    Check(!info.optional_properties.descriptor_heap_properties.pNext && !info.optional_properties.descriptor_buffer_properties.pNext && !info.optional_properties.cooperative_matrix_properties.pNext && !info.optional_properties.cooperative_vector_properties.pNext, "optional property snapshot retained pointers");
    return result;
}
} // namespace

int main() {
    vkEnumerateDeviceExtensionProperties              = Enumerate;
    vkGetPhysicalDeviceFeatures2                      = Features;
    vkGetPhysicalDeviceProperties2                    = Properties;
    vkCreateDevice                                    = Create;
    vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR = MatrixModes;
    vkGetPhysicalDeviceCooperativeVectorPropertiesNV  = VectorModes;
    try {
        Reset();
        VulkanDeviceInfo info;
        Check(Initialize(info) == VK_SUCCESS && enabled == advertised, "minimal device initialization failed");
        advertised.insert(VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME);
        Check(Initialize(info) == VK_SUCCESS && !info.capabilities.m_has_ext_descriptor_heap && !queried.contains(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT), "heap ignored missing maintenance5");
        advertised.insert(VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
        advertised.insert(VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME);
        advertised.insert(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
        enum_incomplete = true;
        Check(Initialize(info) == VK_SUCCESS && info.capabilities.m_has_ext_descriptor_heap && info.capabilities.m_has_ext_descriptor_buffer && info.capabilities.m_has_descriptor_buffer_push, "descriptor capabilities lost");
        Check(info.optional_properties.descriptor_heap_properties.maxPushDataSize == 256 && info.optional_properties.descriptor_buffer_properties.descriptorBufferOffsetAlignment == 64, "descriptor properties lost");
        maintenance5_available = false;
        buffer_available       = false;
        Check(Initialize(info) == VK_SUCCESS && !info.capabilities.m_has_ext_descriptor_heap && !info.capabilities.m_has_ext_descriptor_buffer && !info.capabilities.m_has_descriptor_buffer_push && !created.contains(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT) && !created.contains(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT), "unsupported feature was enabled");
        Reset();
        advertised.insert(VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME);
        advertised.insert(VK_EXT_PAGEABLE_DEVICE_LOCAL_MEMORY_EXTENSION_NAME);
        Check(Initialize(info) == VK_SUCCESS && !info.capabilities.m_has_descriptor_buffer_push && !info.capabilities.m_has_pageable_device_local_memory, "optional dependency was ignored");
        advertised.insert(VK_EXT_MEMORY_PRIORITY_EXTENSION_NAME);
        advertised.insert(VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME);
        advertised.insert(VK_NV_COOPERATIVE_VECTOR_EXTENSION_NAME);
        Check(Initialize(info) == VK_SUCCESS && info.capabilities.m_has_pageable_device_local_memory, "pageable dependency was not enabled");
        Check(info.optional_properties.cooperative_matrix_supports.size() == 1 && info.optional_properties.cooperative_matrix_supports[0].MSize == 16 && info.optional_properties.cooperative_vector_supports.empty(), "cooperative enumeration failure or shrink mishandled");
        create_result = VK_ERROR_INITIALIZATION_FAILED;
        Check(Initialize(info) == create_result, "native creation failure was not propagated");
        advertised.erase(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        const auto before = create_calls;
        Check(Initialize(info) == VK_ERROR_EXTENSION_NOT_PRESENT && create_calls == before, "missing required extension reached creation");
        std::cout << "Vulkan device initialization contracts passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
