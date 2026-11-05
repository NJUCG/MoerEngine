#include "VulkanDeviceInitialization.h"
#include "platform/Platform.h"

#include <algorithm>
#include <string>

namespace Moer::Render::VulkanInitialization {
namespace {

constexpr const char* required_instance_extensions[] = {
    VK_KHR_SURFACE_EXTENSION_NAME,
    VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME,
    VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
#if PLATFORM_WINDOWS
    VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
#endif
};

constexpr const char* required_device_extensions[] = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
#if PLATFORM_WINDOWS && WITH_CUDA
    VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
    VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
#endif
};

template<typename T, typename Function>
void QueryCooperativeModes(VkPhysicalDevice gpu, Function query, Array<T>& modes, VkStructureType type) {
    if (!query)
        return;
    uint32   count  = 0;
    VkResult result = query(gpu, &count, nullptr);
    if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || !count)
        return;
    modes.resize(count);
    for (auto& mode : modes)
        mode.sType = type;
    result = query(gpu, &count, modes.data());
    if (result != VK_SUCCESS && result != VK_INCOMPLETE)
        modes.clear();
    else
        modes.resize(std::min<size_t>(count, modes.size()));
}

} // namespace

std::span<const char* const> RequiredInstanceExtensions() {
    return required_instance_extensions;
}
std::span<const char* const> RequiredDeviceExtensions() {
    return required_device_extensions;
}

VkResult CreateDevice(VkPhysicalDevice gpu, VkDeviceCreateInfo request, VulkanDeviceInfo& info, VkDevice& device) {
    Array<VkExtensionProperties> available;
    VkResult                     result;
    do {
        uint32 count = 0;
        result       = vkEnumerateDeviceExtensionProperties(gpu, nullptr, &count, nullptr);
        if (result != VK_SUCCESS)
            return result;
        available.resize(count);
        result = vkEnumerateDeviceExtensionProperties(gpu, nullptr, &count, available.data());
        if (result != VK_SUCCESS && result != VK_INCOMPLETE)
            return result;
        available.resize(count);
    } while (result == VK_INCOMPLETE);
    const auto supports = [&](std::string_view name) {
        return std::ranges::any_of(available, [&](const auto& extension) {
            return name == extension.extensionName;
        });
    };

    info.enabled_extensions.clear();
    info.capabilities        = {};
    info.optional_properties = {};
    auto& caps               = info.capabilities;
    for (const char* name : required_device_extensions) {
        if (!supports(name))
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        info.enabled_extensions.push_back(name);
    }

    // All nodes belong to this call. Rebuild their links for creation after
    // filtering queried support into the exact set of enabled capabilities.
    VkPhysicalDeviceMaintenance5FeaturesKHR              maintenance5{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR};
    VkPhysicalDeviceDescriptorHeapFeaturesEXT            heap{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT};
    VkPhysicalDeviceDescriptorBufferFeaturesEXT          buffer{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT};
    VkPhysicalDeviceMemoryPriorityFeaturesEXT            priority{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PRIORITY_FEATURES_EXT};
    VkPhysicalDevicePageableDeviceLocalMemoryFeaturesEXT pageable{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PAGEABLE_DEVICE_LOCAL_MEMORY_FEATURES_EXT};
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR         matrix{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR};
    VkPhysicalDeviceCooperativeVectorFeaturesNV          vector{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_VECTOR_FEATURES_NV};
#if PLATFORM_WINDOWS
    VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR    ray_pipeline{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
    VkPhysicalDeviceRayQueryFeaturesKHR              ray_query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
#endif
    VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    const auto                query_feature = [&](const char* name, auto& feature) {
        if (supports(name)) {
            feature.pNext = query.pNext;
            query.pNext   = &feature;
        }
    };
    query_feature(VK_KHR_MAINTENANCE_5_EXTENSION_NAME, maintenance5);
    if (supports(VK_KHR_MAINTENANCE_5_EXTENSION_NAME))
        query_feature(VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME, heap);
    query_feature(VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME, buffer);
    query_feature(VK_EXT_MEMORY_PRIORITY_EXTENSION_NAME, priority);
    query_feature(VK_EXT_PAGEABLE_DEVICE_LOCAL_MEMORY_EXTENSION_NAME, pageable);
    query_feature(VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME, matrix);
    query_feature(VK_NV_COOPERATIVE_VECTOR_EXTENSION_NAME, vector);
#if PLATFORM_WINDOWS
    query_feature(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, acceleration);
    query_feature(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME, ray_pipeline);
    query_feature(VK_KHR_RAY_QUERY_EXTENSION_NAME, ray_query);
#endif
    vkGetPhysicalDeviceFeatures2(gpu, &query);

    void*      enabled_chain  = const_cast<void*>(request.pNext);
    const auto enable_feature = [&](const char* name, auto& feature, bool usable) {
        if (!supports(name) || !usable)
            return false;
        feature.pNext = enabled_chain;
        enabled_chain = &feature;
        info.enabled_extensions.push_back(name);
        return true;
    };
    const auto enable_extension = [&](const char* name) {
        if (!supports(name))
            return false;
        info.enabled_extensions.push_back(name);
        return true;
    };
    caps.m_has_khr_maintenance5      = enable_feature(VK_KHR_MAINTENANCE_5_EXTENSION_NAME, maintenance5, maintenance5.maintenance5);
    heap.descriptorHeapCaptureReplay = VK_FALSE;
    caps.m_has_ext_descriptor_heap   = enable_feature(VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME, heap, heap.descriptorHeap && caps.m_has_khr_maintenance5);
    const bool push_descriptors      = enable_extension(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
    if (!push_descriptors)
        buffer.descriptorBufferPushDescriptors = VK_FALSE;
    caps.m_has_ext_descriptor_buffer                       = enable_feature(VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME, buffer, buffer.descriptorBuffer);
    caps.m_has_descriptor_buffer_push                      = caps.m_has_ext_descriptor_buffer && buffer.descriptorBufferPushDescriptors;
    caps.m_has_memory_priority                             = enable_feature(VK_EXT_MEMORY_PRIORITY_EXTENSION_NAME, priority, priority.memoryPriority);
    caps.m_has_pageable_device_local_memory                = enable_feature(VK_EXT_PAGEABLE_DEVICE_LOCAL_MEMORY_EXTENSION_NAME, pageable, pageable.pageableDeviceLocalMemory && caps.m_has_memory_priority);
    caps.m_has_khr_cooperative_matrix                      = enable_feature(VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME, matrix, matrix.cooperativeMatrix);
    caps.m_has_khr_cooperative_matrix_robust_buffer_access = caps.m_has_khr_cooperative_matrix && matrix.cooperativeMatrixRobustBufferAccess;
    caps.m_has_nv_cooperative_vector                       = enable_feature(VK_NV_COOPERATIVE_VECTOR_EXTENSION_NAME, vector, vector.cooperativeVector);
    caps.m_has_nv_cooperative_vector_training              = caps.m_has_nv_cooperative_vector && vector.cooperativeVectorTraining;
#if PLATFORM_WINDOWS
    const bool deferred_host              = enable_extension(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
    caps.m_has_khr_acceleration_structure = enable_feature(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, acceleration, deferred_host && acceleration.accelerationStructure && acceleration.descriptorBindingAccelerationStructureUpdateAfterBind);
    caps.m_has_khr_ray_tracing_pipeline   = enable_feature(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME, ray_pipeline, caps.m_has_khr_acceleration_structure && ray_pipeline.rayTracingPipeline && ray_pipeline.rayTraversalPrimitiveCulling);
    caps.m_has_khr_ray_query              = enable_feature(VK_KHR_RAY_QUERY_EXTENSION_NAME, ray_query, caps.m_has_khr_acceleration_structure && ray_query.rayQuery);
#endif

    const auto& core11_snapshot                        = info.core_features.GetCore11Features();
    const auto& core12_snapshot                        = info.core_features.GetCore12Features();
    caps.m_has_shader_float16                          = core12_snapshot.shaderFloat16;
    caps.m_has_shader_int8                             = core12_snapshot.shaderInt8;
    caps.m_has_vulkan_memory_model                     = core12_snapshot.vulkanMemoryModel;
    caps.m_has_storage_buffer_16bit_access             = core11_snapshot.storageBuffer16BitAccess;
    caps.m_has_uniform_and_storage_buffer_16bit_access = core11_snapshot.uniformAndStorageBuffer16BitAccess;
    caps.m_has_storage_buffer_8bit_access              = core12_snapshot.storageBuffer8BitAccess;
    caps.m_has_uniform_and_storage_buffer_8bit_access  = core12_snapshot.uniformAndStorageBuffer8BitAccess;

    VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    const auto                  query_property = [&](auto& property, VkStructureType type, bool enabled) {
        if (enabled) {
            property.sType   = type;
            property.pNext   = properties.pNext;
            properties.pNext = &property;
        }
    };
    auto& optional = info.optional_properties;
    query_property(optional.descriptor_heap_properties, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT, caps.m_has_ext_descriptor_heap);
    query_property(optional.descriptor_buffer_properties, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_PROPERTIES_EXT, caps.m_has_ext_descriptor_buffer);
    query_property(optional.push_descriptor_properties, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR, push_descriptors);
    query_property(optional.acceleration_structure_properties, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR, caps.m_has_khr_acceleration_structure);
    query_property(optional.ray_tracing_pipeline_properties, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR, caps.m_has_khr_ray_tracing_pipeline);
    query_property(optional.cooperative_matrix_properties, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_PROPERTIES_KHR, caps.m_has_khr_cooperative_matrix);
    query_property(optional.cooperative_vector_properties, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_VECTOR_PROPERTIES_NV, caps.m_has_nv_cooperative_vector);
    vkGetPhysicalDeviceProperties2(gpu, &properties);
    for (auto* node = static_cast<VkBaseOutStructure*>(properties.pNext); node;) {
        auto* next  = node->pNext;
        node->pNext = nullptr;
        node        = next;
    }
    if (caps.m_has_khr_cooperative_matrix)
        QueryCooperativeModes(gpu, vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR, optional.cooperative_matrix_supports, VK_STRUCTURE_TYPE_COOPERATIVE_MATRIX_PROPERTIES_KHR);
    if (caps.m_has_nv_cooperative_vector)
        QueryCooperativeModes(gpu, vkGetPhysicalDeviceCooperativeVectorPropertiesNV, optional.cooperative_vector_supports, VK_STRUCTURE_TYPE_COOPERATIVE_VECTOR_PROPERTIES_NV);

    auto core11                     = info.core_features.GetCore11Features();
    auto core12                     = info.core_features.GetCore12Features();
    auto core13                     = info.core_features.GetCore13Features();
    core11.pNext                    = &core12;
    core12.pNext                    = &core13;
    core13.pNext                    = enabled_chain;
    request.pNext                   = &core11;
    request.pEnabledFeatures        = &info.core_features.GetCore10Features();
    request.enabledExtensionCount   = static_cast<uint32>(info.enabled_extensions.size());
    request.ppEnabledExtensionNames = info.enabled_extensions.data();
    return vkCreateDevice(gpu, &request, nullptr, &device);
}

} // namespace Moer::Render::VulkanInitialization
