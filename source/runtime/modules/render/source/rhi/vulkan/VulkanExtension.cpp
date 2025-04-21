//
// Created by 74535 on 2023/10/11.
//

#include "rhi/RHI.h"

#include <volk.h>
#include "VulkanDevice.h"
#include "VulkanPlatform.h"
#include "VulkanExtension.h"
#include "VulkanMacroUtils.h"

template<typename ExistingChainType, typename NewStructType>
static void AddToPNext(ExistingChainType& _existing, NewStructType& _added) {
    _added.pNext    = (void*)_existing.pNext;
    _existing.pNext = (void*)&_added;
}

namespace Moer::Render {

    TExtensionArray VulkanInstanceExtension::GetMERequiredInstanceExtensions() {
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

    // ***** VK_KHR_acceleration_structure
    class VulkanKHRAccelerationStructureExtension final : public VulkanDeviceExtension {
    public:
        VulkanKHRAccelerationStructureExtension(bool _is_optional = false)
            : VulkanDeviceExtension(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, _is_optional), m_acceleration_structure_features() {}

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
        VulkanKHRRayTracingPipelineExtension(bool _is_optional = false)
            : VulkanDeviceExtension(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME, _is_optional), m_ray_tracing_pipeline_features() {}

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
        VulkanKHRRayQueryExtension(bool _is_optional = false)
            : VulkanDeviceExtension(VK_KHR_RAY_QUERY_EXTENSION_NAME, _is_optional), m_ray_query_features() {}

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
        VulkanEXTDescriptorBufferExtension(bool _is_optional = false)
            : VulkanDeviceExtension(VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME, _is_optional), m_descriptor_buffer_features() {}

        void PreGpuFeatures(VkPhysicalDeviceFeatures2& _gpu_features2) override {
            m_descriptor_buffer_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT;
            AddToPNext(_gpu_features2, m_descriptor_buffer_features);
        }

        void PostGpuFeatures(VulkanOptionalDeviceExtensions& _gpu_extensions) override {
            m_is_usable = (m_descriptor_buffer_features.descriptorBuffer == VK_TRUE && m_descriptor_buffer_features.descriptorBufferPushDescriptors == VK_TRUE);

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

    //***** VK_KHR_push_descriptor */
    class VulkanKHRPushDescriptorExtension final : public VulkanDeviceExtension {
    public:
        VulkanKHRPushDescriptorExtension(bool _is_optional = false)
            : VulkanDeviceExtension(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, _is_optional) {}

        void PreGpuFeatures(VkPhysicalDeviceFeatures2& _gpu_features2) override {
        }

        void PostGpuFeatures(VulkanOptionalDeviceExtensions& _gpu_extensions) override {
        }

        void PreGpuProperties(const VulkanDevice* _device, VkPhysicalDeviceProperties2& _gpu_properties2) override {
            auto& push_descriptor_props = const_cast<VulkanOptionalDeviceProperties&>(_device->GetOptionalProperties()).push_descriptor_properties;
            push_descriptor_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR;
            AddToPNext(_gpu_properties2, push_descriptor_props);
        }

        void PreCreateDevice(VkDeviceCreateInfo& _device_create_info) override {
        }

    private:
    };

    class VulkanEXTMemoryDecompressionExtension final : public VulkanDeviceExtension {
    public:
        VulkanEXTMemoryDecompressionExtension(bool _is_optional = true)
            : VulkanDeviceExtension(VK_NV_MEMORY_DECOMPRESSION_EXTENSION_NAME, _is_optional), m_memory_decompression_features{} {}

        void PreGpuFeatures(VkPhysicalDeviceFeatures2& _gpu_features2) override {
            m_memory_decompression_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_DECOMPRESSION_FEATURES_NV;
            AddToPNext(_gpu_features2, m_memory_decompression_features);
        }

        void PostGpuFeatures(VulkanOptionalDeviceExtensions& _gpu_extensions) override {
            m_is_usable = (m_memory_decompression_features.memoryDecompression == VK_TRUE);

            _gpu_extensions.m_has_nv_memory_decompression = m_is_usable;
        }

        void PreGpuProperties(const VulkanDevice* _device, VkPhysicalDeviceProperties2& _gpu_properties2) override {
            auto& memory_decompression = const_cast<VulkanOptionalDeviceProperties&>(_device->GetOptionalProperties()).memory_decompression_properties;
            memory_decompression.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_DECOMPRESSION_PROPERTIES_NV;
            AddToPNext(_gpu_properties2, memory_decompression);
        }

        void PreCreateDevice(VkDeviceCreateInfo& _device_create_info) override {
            if (m_is_usable && m_is_enabled) {
                AddToPNext(_device_create_info, m_memory_decompression_features);
            }
        }

    private:
        VkPhysicalDeviceMemoryDecompressionFeaturesNV m_memory_decompression_features;
    };

    class VulkanEXTCopyMemoryIndirectExtension final : public VulkanDeviceExtension {
    public:
        VulkanEXTCopyMemoryIndirectExtension(bool _is_optional = true)
            : VulkanDeviceExtension(VK_NV_COPY_MEMORY_INDIRECT_EXTENSION_NAME, _is_optional), m_copy_memory_indirect_features() {}

        void PreGpuFeatures(VkPhysicalDeviceFeatures2& _gpu_features2) override {
            m_copy_memory_indirect_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COPY_MEMORY_INDIRECT_FEATURES_NV;
            AddToPNext(_gpu_features2, m_copy_memory_indirect_features);
        }

        void PostGpuFeatures(VulkanOptionalDeviceExtensions& _gpu_extensions) override {
            m_is_usable = (m_copy_memory_indirect_features.indirectCopy == VK_TRUE);

            _gpu_extensions.m_has_nv_copy_memory_indirect = m_is_usable;
        }

        void PreGpuProperties(const VulkanDevice* _device, VkPhysicalDeviceProperties2& _gpu_properties2) override {
            auto& copy_memory_indirect = const_cast<VulkanOptionalDeviceProperties&>(_device->GetOptionalProperties()).copy_memory_indirect_properties;
            copy_memory_indirect.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COPY_MEMORY_INDIRECT_PROPERTIES_NV;
            AddToPNext(_gpu_properties2, copy_memory_indirect);
        }

        void PreCreateDevice(VkDeviceCreateInfo& _device_create_info) override {
            if (m_is_usable && m_is_enabled) {
                AddToPNext(_device_create_info, m_copy_memory_indirect_features);
            }
        }

    private:
        VkPhysicalDeviceCopyMemoryIndirectFeaturesNV m_copy_memory_indirect_features;
    };

    class VulkanEXTMemoryPriorityAllocateInfoExtension final : public VulkanDeviceExtension {
    public:
        VulkanEXTMemoryPriorityAllocateInfoExtension(bool _is_optional = true)
            : VulkanDeviceExtension(VK_EXT_MEMORY_PRIORITY_EXTENSION_NAME, _is_optional), m_memory_priority_features() {}

        void PreGpuFeatures(VkPhysicalDeviceFeatures2& _gpu_features2) override {
            m_memory_priority_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PRIORITY_FEATURES_EXT;
            AddToPNext(_gpu_features2, m_memory_priority_features);
        }

        void PostGpuFeatures(VulkanOptionalDeviceExtensions& _gpu_extensions) override {
            m_is_usable = (m_memory_priority_features.memoryPriority == VK_TRUE);

            _gpu_extensions.m_has_memory_priority = m_is_usable;
        }

        void PreCreateDevice(VkDeviceCreateInfo& _device_create_info) override {
            if (m_is_usable && m_is_enabled) {
                AddToPNext(_device_create_info, m_memory_priority_features);
            }
        }

    private:
        VkPhysicalDeviceMemoryPriorityFeaturesEXT m_memory_priority_features;
    };

    class VulkanEXTPageableDeviceLocalMemoryExtension final : public VulkanDeviceExtension {
    public:
        VulkanEXTPageableDeviceLocalMemoryExtension(bool _is_optional = true)
            : VulkanDeviceExtension(VK_EXT_PAGEABLE_DEVICE_LOCAL_MEMORY_EXTENSION_NAME, _is_optional), m_pageable_device_local_memory_features() {}

        void PreGpuFeatures(VkPhysicalDeviceFeatures2& _gpu_features2) override {
            m_pageable_device_local_memory_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PAGEABLE_DEVICE_LOCAL_MEMORY_FEATURES_EXT;
            AddToPNext(_gpu_features2, m_pageable_device_local_memory_features);
        }

        void PostGpuFeatures(VulkanOptionalDeviceExtensions& _gpu_extensions) override {
            m_is_usable = (m_pageable_device_local_memory_features.pageableDeviceLocalMemory == VK_TRUE);

            _gpu_extensions.m_has_pageable_device_local_memory = m_is_usable;
        }

        void PreCreateDevice(VkDeviceCreateInfo& _device_create_info) override {
            if (m_is_usable && m_is_enabled) {
                AddToPNext(_device_create_info, m_pageable_device_local_memory_features);
            }
        }

    private:
        VkPhysicalDevicePageableDeviceLocalMemoryFeaturesEXT m_pageable_device_local_memory_features;
    };

    class VulkanEXTMeshShaderExtension final : public VulkanDeviceExtension {
    public:
        VulkanEXTMeshShaderExtension(bool _is_optional = true)
            : VulkanDeviceExtension(VK_EXT_MESH_SHADER_EXTENSION_NAME, _is_optional), m_mesh_shader_features() {}

        void PreGpuFeatures(VkPhysicalDeviceFeatures2& _gpu_features2) override {
            m_mesh_shader_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;
            AddToPNext(_gpu_features2, m_mesh_shader_features);
        }

        void PostGpuFeatures(VulkanOptionalDeviceExtensions& _gpu_extensions) override {
            m_is_usable = (m_mesh_shader_features.meshShader == VK_TRUE);

            _gpu_extensions.m_has_ext_mesh_shader          = m_is_usable;
            _gpu_extensions.m_allow_mesh_primitive_shading = (m_mesh_shader_features.primitiveFragmentShadingRateMeshShader == VK_TRUE);
        }

        void PreGpuProperties(const VulkanDevice* _device, VkPhysicalDeviceProperties2& _gpu_properties2) override {
            auto& mesh_shader_props = const_cast<VulkanOptionalDeviceProperties&>(_device->GetOptionalProperties()).mesh_shader_properties;
            mesh_shader_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT;
            AddToPNext(_gpu_properties2, mesh_shader_props);
        }

        void PreCreateDevice(VkDeviceCreateInfo& _device_create_info) override {
            if (m_is_usable && m_is_enabled) {
                AddToPNext(_device_create_info, m_mesh_shader_features);
            }
        }

    private:
        VkPhysicalDeviceMeshShaderFeaturesEXT m_mesh_shader_features;
    };

    TVulkanDeviceExtensionArray VulkanDeviceExtension::GetMERequiredDeviceExtensions() {
        // LOG_INFO("VulkanDeviceExtension: raytracing support, {}", _rhi_info.ray_tracing);

        TVulkanDeviceExtensionArray extensions;

#define ADD_EXTENSION(ext_name, optional) extensions.emplace_back(std::make_shared<VulkanDeviceExtension>(ext_name, optional))

#define ADD_CUSTOM_EXTENSION(ext_class, optional) extensions.emplace_back(std::make_shared<ext_class>(optional))
        // generic simple extensions
        ADD_EXTENSION(VK_KHR_SWAPCHAIN_EXTENSION_NAME, VULKAN_EXTENSION_REQUIRED);
        // ADD_EXTENSION(VK_KHR_INDEX_TYPE_UINT8_EXTENSION_NAME, VULKAN_EXTENSION_REQUIRED);

#if VULKAN_RHI_RAYTRACING
        // raytracing extensions
        ADD_EXTENSION(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME, VULKAN_EXTENSION_OPTIONAL);
        ADD_CUSTOM_EXTENSION(VulkanKHRAccelerationStructureExtension, VULKAN_EXTENSION_OPTIONAL);
        ADD_CUSTOM_EXTENSION(VulkanKHRRayTracingPipelineExtension, VULKAN_EXTENSION_OPTIONAL);
        ADD_CUSTOM_EXTENSION(VulkanKHRRayQueryExtension, VULKAN_EXTENSION_OPTIONAL);
#endif
        // bindless extensions
        ADD_CUSTOM_EXTENSION(VulkanEXTDescriptorBufferExtension, VULKAN_EXTENSION_OPTIONAL);
        ADD_CUSTOM_EXTENSION(VulkanKHRPushDescriptorExtension, VULKAN_EXTENSION_REQUIRED);
        // vendor extensions

        // debug extensions

        // platform specific extensions
        // VulkanPlatform::GetDeviceExtensions(extensions);//MARK...
        ADD_CUSTOM_EXTENSION(VulkanEXTMemoryPriorityAllocateInfoExtension, VULKAN_EXTENSION_OPTIONAL);
        ADD_CUSTOM_EXTENSION(VulkanEXTPageableDeviceLocalMemoryExtension, VULKAN_EXTENSION_OPTIONAL);
        // nvidia extensions
        ADD_CUSTOM_EXTENSION(VulkanEXTMemoryDecompressionExtension, VULKAN_EXTENSION_OPTIONAL);
        ADD_CUSTOM_EXTENSION(VulkanEXTCopyMemoryIndirectExtension, VULKAN_EXTENSION_OPTIONAL);

#undef ADD_EXTENSION
#undef ADD_CUSTOM_EXTENSION

        return extensions;
    }

    TVulkanDeviceExtensionArray VulkanDeviceExtension::GetMEEnabledDeviceExtensions(const Set<std::string>& _gpu_extensions) {
        auto extensions = GetMERequiredDeviceExtensions();

        TVulkanDeviceExtensionArray extensions_enabled;

        for (const auto& ext : extensions) {
            if (_gpu_extensions.contains(ext->GetExtensionName().data())) {
                ext->Enable();
                extensions_enabled.emplace_back(ext);
            }
        }

        return extensions_enabled;
    }
}// namespace Moer::Render