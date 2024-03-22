//
// Created by 74535 on 2023/10/11.
//

#include "rhi/RHI.h"

#include "VulkanDevice.h"
#include "VulkanPlatform.h"
#include "VulkanExtension.h"
#include "rhi/vulkan/misc/VulkanMacroUtils.h"

#include <vulkan/vulkan_core.h>

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
    // ADD_EXTENSION(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
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
class VulkanKHRAccelerationStructureExtension final : public VulkanDeviceExtension {
public:
    VulkanKHRAccelerationStructureExtension(bool _is_enabled, bool _is_optional = false)
        : VulkanDeviceExtension(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, _is_enabled, _is_optional), m_acceleration_structure_features() {}

    void PreGpuFeatures(VkPhysicalDeviceFeatures2& _gpu_features2) override {
        m_acceleration_structure_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
        AddToPNext(_gpu_features2, m_acceleration_structure_features);
    }

    void PostGpuFeatures(VulkanOptionalDeviceExtensions& _gpu_extensions) override {
        m_is_usable = (m_acceleration_structure_features.accelerationStructure == VK_TRUE) &&
                      (m_acceleration_structure_features.descriptorBindingAccelerationStructureUpdateAfterBind == VK_TRUE);
        _gpu_extensions.m_has_khr_acceleration_structure = m_is_usable;
    }

    void PreGpuProperties(const VulkanDevice* _device, VkPhysicalDeviceProperties2& _gpu_properties2) override {
        auto& acceleration_structure_props = const_cast<VulkanOptionalDeviceProperties&>(_device->GetOptionalProperties()).acceleration_structure_properties;
        acceleration_structure_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
        AddToPNext(_gpu_properties2, acceleration_structure_props);
    }

    void PreCreateDevice(VkDeviceCreateInfo& _device_create_info) override {
        if (m_is_usable && m_is_enabled) {
            AddToPNext(_device_create_info, m_acceleration_structure_features);
        }
    }

private:
    VkPhysicalDeviceAccelerationStructureFeaturesKHR m_acceleration_structure_features;
};

// ***** VK_KHR_ray_tracing_pipeline
class VulkanKHRRayTracingPipelineExtension final : public VulkanDeviceExtension {
public:
    VulkanKHRRayTracingPipelineExtension(bool _is_enabled, bool _is_optional = false)
        : VulkanDeviceExtension(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME, _is_enabled, _is_optional), m_ray_tracing_pipeline_features() {}

    void PreGpuFeatures(VkPhysicalDeviceFeatures2& _gpu_features2) override {
        m_ray_tracing_pipeline_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
        AddToPNext(_gpu_features2, m_ray_tracing_pipeline_features);
    }

    void PostGpuFeatures(VulkanOptionalDeviceExtensions& _gpu_extensions) override {
        m_is_usable = (m_ray_tracing_pipeline_features.rayTracingPipeline == VK_TRUE) &&
                      (m_ray_tracing_pipeline_features.rayTraversalPrimitiveCulling == VK_TRUE);

        _gpu_extensions.m_has_khr_ray_tracing_pipeline = m_is_usable;
    }

    void PreGpuProperties(const VulkanDevice* _device, VkPhysicalDeviceProperties2& _gpu_properties2) override {
        auto& ray_tracing_pipeline_props = const_cast<VulkanOptionalDeviceProperties&>(_device->GetOptionalProperties()).ray_tracing_pipeline_properties;
        ray_tracing_pipeline_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;
        AddToPNext(_gpu_properties2, ray_tracing_pipeline_props);
    }

    void PreCreateDevice(VkDeviceCreateInfo& _device_create_info) override {
        if (m_is_usable && m_is_enabled) {
            AddToPNext(_device_create_info, m_ray_tracing_pipeline_features);
        }
    }

private:
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR m_ray_tracing_pipeline_features;
};

// ***** VK_KHR_ray_query
class VulkanKHRRayQueryExtension final : public VulkanDeviceExtension {
public:
    VulkanKHRRayQueryExtension(bool _is_enabled, bool _is_optional = false)
        : VulkanDeviceExtension(VK_KHR_RAY_QUERY_EXTENSION_NAME, _is_enabled, _is_optional), m_ray_query_features() {}

    void PreGpuFeatures(VkPhysicalDeviceFeatures2& _gpu_features2) override {
        m_ray_query_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
        AddToPNext(_gpu_features2, m_ray_query_features);
    }

    void PostGpuFeatures(VulkanOptionalDeviceExtensions& _gpu_extensions) override {
        m_is_usable = (m_ray_query_features.rayQuery == VK_TRUE);

        _gpu_extensions.m_has_khr_ray_query = m_is_usable;
    }

    void PreCreateDevice(VkDeviceCreateInfo& _device_create_info) override {
        if (m_is_usable && m_is_enabled) {
            AddToPNext(_device_create_info, m_ray_query_features);
        }
    }

private:
    VkPhysicalDeviceRayQueryFeaturesKHR m_ray_query_features;
};

// ***** VK_EXT_descriptor_buffer
class VulkanEXTDescriptorBufferExtension final : public VulkanDeviceExtension {
public:
    VulkanEXTDescriptorBufferExtension(bool _is_enabled = true, bool _is_optional = false)
        : VulkanDeviceExtension(VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME, _is_enabled, _is_optional), m_descriptor_buffer_features() {}

    void PreGpuFeatures(VkPhysicalDeviceFeatures2& _gpu_features2) override {
        m_descriptor_buffer_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT;
        AddToPNext(_gpu_features2, m_descriptor_buffer_features);
    }

    void PostGpuFeatures(VulkanOptionalDeviceExtensions& _gpu_extensions) override {
        m_is_usable = (m_descriptor_buffer_features.descriptorBuffer == VK_TRUE);

        _gpu_extensions.m_has_ext_descriptor_buffer = m_is_usable;
    }

    void PreGpuProperties(const VulkanDevice* _device, VkPhysicalDeviceProperties2& _gpu_properties2) override {
        auto& descriptor_buffer_props = const_cast<VulkanOptionalDeviceProperties&>(_device->GetOptionalProperties()).descriptor_buffer_properties;
        descriptor_buffer_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_PROPERTIES_EXT;
        AddToPNext(_gpu_properties2, descriptor_buffer_props);
    }

    void PreCreateDevice(VkDeviceCreateInfo& _device_create_info) override {
        if (m_is_usable && m_is_enabled) {
            AddToPNext(_device_create_info, m_descriptor_buffer_features);
        }
    }

private:
    VkPhysicalDeviceDescriptorBufferFeaturesEXT m_descriptor_buffer_features;
};

TVulkanDeviceExtensionArray VulkanDeviceExtension::GetMESupportedDeviceExtensions(const RHIInfo& _rhi_info) {
    LOG_INFO("VulkanDeviceExtension: raytracing support, {}", _rhi_info.ray_tracing);
    TVulkanDeviceExtensionArray extensions;

#define ADD_EXTENSION(ext_name, enabled, optional) extensions.emplace_back(std::make_shared<VulkanDeviceExtension>(ext_name, enabled, optional))

#define ADD_CUSTOM_EXTENSION(ext_class, enabled, optional) extensions.emplace_back(std::make_shared<ext_class>(enabled, optional))
    // generic simple extensions
    ADD_EXTENSION(VK_KHR_SWAPCHAIN_EXTENSION_NAME, VULKAN_EXTENSION_ENABLED, VULKAN_EXTENSION_REQUIRED);
    ADD_EXTENSION(VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME, VULKAN_EXTENSION_ENABLED, VULKAN_EXTENSION_REQUIRED);

    // raytracing extensions
    ADD_EXTENSION(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME, _rhi_info.ray_tracing, VULKAN_EXTENSION_OPTIONAL);
    ADD_CUSTOM_EXTENSION(VulkanKHRAccelerationStructureExtension, _rhi_info.ray_tracing, VULKAN_EXTENSION_OPTIONAL);
    ADD_CUSTOM_EXTENSION(VulkanKHRRayTracingPipelineExtension, _rhi_info.ray_tracing, VULKAN_EXTENSION_OPTIONAL);
    ADD_CUSTOM_EXTENSION(VulkanKHRRayQueryExtension, _rhi_info.ray_tracing, VULKAN_EXTENSION_OPTIONAL);

    // bindless extensions
    ADD_CUSTOM_EXTENSION(VulkanEXTDescriptorBufferExtension, VULKAN_EXTENSION_ENABLED, VULKAN_EXTENSION_OPTIONAL);

    // vendor extensions

    // debug extensions

    // platform specific extensions
    VulkanPlatform::GetDeviceExtensions(extensions);

#undef ADD_EXTENSION
#undef ADD_CUSTOM_EXTENSION

    return extensions;
}

void VulkanEnabledDeviceExtensions::Init(const TVulkanDeviceExtensionArray& _enabled_extensions) {
    for (const auto& ext : _enabled_extensions) {
        if (ext->IsEnabled()) {
            m_enabled_extensions.push_back(ext);
        }
    }
}