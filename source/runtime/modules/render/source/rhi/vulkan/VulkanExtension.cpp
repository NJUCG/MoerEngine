//
// Created by 74535 on 2023/10/11.
//

#include "VulkanExtension.h"
#include "VulkanPlatform.h"
#include "rhi/vulkan/misc/VulkanMacroUtils.h"
#include "vulkan/vulkan_core.h"

template<typename ExistingChainType, typename NewStructType>
static void AddToPNext(ExistingChainType& _existing, NewStructType& _added) {
    _added.pNext    = (void*)_existing.pNext;
    _existing.pNext = (void*)&_added;
}

TExtensionArray VulkanInstanceExtension::GetMESupportedInstanceExtensions() {
    TExtensionArray extensions;

#define ADD_EXTENSION(ext_name) extensions.push_back(ext_name)

    // generic simple extensions
    ADD_EXTENSION(VK_KHR_SURFACE_EXTENSION_NAME);
    ADD_EXTENSION(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
    ADD_EXTENSION(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
    // debug utils, contains debug marker and debug report
    ADD_EXTENSION(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    // platform specific extensions
    VulkanPlatform::GetInstanceExtensions(extensions);

    // custom extensions

#undef ADD_EXTENSION

    return extensions;
}

TExtensionPropsArray VulkanInstanceExtension::GetDriverSupportedInstanceExtensions(const char* _layer_name) {
    TExtensionPropsArray ext_props;

    uint32_t prop_count = 0;
    VK_CHECK_RESULT(vkEnumerateInstanceExtensionProperties(_layer_name, &prop_count, nullptr));
    if (prop_count > 0) {
        ext_props.resize(prop_count);
        VK_CHECK_RESULT(vkEnumerateInstanceExtensionProperties(_layer_name, &prop_count, ext_props.data()));
    }

    return ext_props;
}

TExtensionArray VulkanInstanceExtension::GetDriverSupportedInstanceExtensionNames(const char* _layer_name) {
    TExtensionPropsArray ext_props;
    TExtensionArray      extensions;

    uint32_t prop_count = 0;
    VK_CHECK_RESULT(vkEnumerateInstanceExtensionProperties(_layer_name, &prop_count, nullptr));
    if (prop_count > 0) {
        ext_props.resize(prop_count);
        VK_CHECK_RESULT(vkEnumerateInstanceExtensionProperties(_layer_name, &prop_count, ext_props.data()));
        for (const auto& prop : ext_props) {
            extensions.push_back(prop.extensionName);
        }
    }

    return extensions;
}

TExtensionPropsArray VulkanDeviceExtension::GetDriverSupportedDeviceExtensions(VkPhysicalDevice _gpu, const char* _layer_name) {
    TExtensionPropsArray ext_props;

    uint32_t prop_count = 0;
    VK_CHECK_RESULT(vkEnumerateDeviceExtensionProperties(_gpu, _layer_name, &prop_count, nullptr));
    if (prop_count > 0) {
        ext_props.resize(prop_count);
        VK_CHECK_RESULT(vkEnumerateDeviceExtensionProperties(_gpu, _layer_name, &prop_count, ext_props.data()));
    }

    return ext_props;
}

TExtensionArray VulkanDeviceExtension::GetDriverSupportedDeviceExtensionNames(VkPhysicalDevice _gpu, const char* _layer_name) {
    TExtensionPropsArray ext_props;
    TExtensionArray      extensions;

    uint32_t prop_count = 0;
    VK_CHECK_RESULT(vkEnumerateDeviceExtensionProperties(_gpu, _layer_name, &prop_count, nullptr));
    if (prop_count > 0) {
        ext_props.resize(prop_count);
        VK_CHECK_RESULT(vkEnumerateDeviceExtensionProperties(_gpu, _layer_name, &prop_count, ext_props.data()));
        for (const auto& prop : ext_props) {
            extensions.push_back(prop.extensionName);
        }
    }

    return extensions;
}

// ***** VK_KHR_acceleration_structure
class VulkanKHRAccelerationStructureExtension : public VulkanDeviceExtension {
public:
    VulkanKHRAccelerationStructureExtension()
        : VulkanDeviceExtension(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) {}

    bool IsOptional() const override { return true; }

    void PreGpuFeatures(VkPhysicalDeviceFeatures2& _gpu_features2) override final {
        AddToPNext(_gpu_features2, m_acceleration_structure_features);
    }

    void PreGpuProperties(VkPhysicalDeviceProperties2& _gpu_properties2) override final {
        AddToPNext(_gpu_properties2, m_acceleration_structure_props);
    }

    void PreCreateDevice(VkDeviceCreateInfo& _device_create_info) override final {
        AddToPNext(_device_create_info, m_acceleration_structure_features);
    }

private:
    VkPhysicalDeviceAccelerationStructurePropertiesKHR m_acceleration_structure_props;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR   m_acceleration_structure_features;
};

// ***** VK_KHR_ray_tracing_pipeline
class VulkanKHRRayTracingPipelineExtension : public VulkanDeviceExtension {
public:
    VulkanKHRRayTracingPipelineExtension()
        : VulkanDeviceExtension(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME) {}

    bool IsOptional() const override { return true; }

    void PreGpuFeatures(VkPhysicalDeviceFeatures2& _gpu_features2) override final {
        AddToPNext(_gpu_features2, m_ray_tracing_pipeline_features);
    }

    void PreGpuProperties(VkPhysicalDeviceProperties2& _gpu_properties2) override final {
        AddToPNext(_gpu_properties2, m_ray_tracing_pipeline_props);
    }
    void PreCreateDevice(VkDeviceCreateInfo& _device_create_info) override final {
        AddToPNext(_device_create_info, m_ray_tracing_pipeline_features);
    }

private:
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR m_ray_tracing_pipeline_props;
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR   m_ray_tracing_pipeline_features;
};

// ***** VK_KHR_ray_query
class VulkanKHRRayQueryExtension : public VulkanDeviceExtension {
public:
    VulkanKHRRayQueryExtension()
        : VulkanDeviceExtension(VK_KHR_RAY_QUERY_EXTENSION_NAME) {}

    bool IsOptional() const override { return true; }

    void PreGpuFeatures(VkPhysicalDeviceFeatures2& _gpu_features2) override final {
        AddToPNext(_gpu_features2, m_ray_query_features);
    }

    void PreCreateDevice(VkDeviceCreateInfo& _device_create_info) override final {
        AddToPNext(_device_create_info, m_ray_query_features);
    }

private:
    VkPhysicalDeviceRayQueryFeaturesKHR m_ray_query_features;
};

//class VulkanKHRTimelineSemaphoreExtension : public VulkanDeviceExtension {
//public:
//    VulkanKHRTimelineSemaphoreExtension()
//        : VulkanDeviceExtension(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME),
//          m_timeline_semaphore_features(
//              VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES_KHR,
//              VK_NULL_HANDLE,
//              VK_TRUE) {}
//
//    void PreGpuFeatures(VkPhysicalDeviceFeatures2& _gpu_features2) override final {
//        AddToPNext(_gpu_features2, m_timeline_semaphore_features);
//    }
//    void PreGpuProperties(VkPhysicalDeviceProperties2& _gpu_properties2) override {
//    }
//    void PreCreateDevice(VkDeviceCreateInfo& _device_create_info) override final {
//        AddToPNext(_device_create_info, m_timeline_semaphore_features);
//    }
//
//private:
//    VkPhysicalDeviceTimelineSemaphoreFeatures m_timeline_semaphore_features;
//};

TVulkanDeviceExtensionArray VulkanDeviceExtension::GetMESupportedDeviceExtensions() {
    TVulkanDeviceExtensionArray extensions;

#define ADD_EXTENSION(ext_name) extensions.emplace_back(std::make_unique<VulkanDeviceExtension>(ext_name))
    // generic simple extensions
    ADD_EXTENSION(VK_KHR_SWAPCHAIN_EXTENSION_NAME);

    // raytracing extensions

    // vendor extensions

    // debug extensions

    // platform specific extensions
    VulkanPlatform::GetDeviceExtensions(extensions);

#undef ADD_EXTENSION

    return extensions;
}