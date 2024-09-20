//
// Created by 74535 on 2023/10/11.
//

#ifndef VULKAN_EXTENSION_H
#define VULKAN_EXTENSION_H

#include <string_view>
#define VULKAN_EXTENSION_OPTIONAL 1
#define VULKAN_EXTENSION_REQUIRED 0

#include "VulkanTypeDefs.h"
#include "log/LogSystem.h"

struct RHIInfo;
namespace Moer::Render {

    class VulkanDevice;
    class VulkanOptionalDeviceExtensions;

    class VulkanExtensionBase {
    public:
        VulkanExtensionBase(std::string_view _ext_name) : m_extension_name(_ext_name), m_is_enabled(false) {}
        virtual ~VulkanExtensionBase() = default;

        inline const std::string_view GetExtensionName() const { return m_extension_name; }

        inline void Enable() { m_is_enabled = true; }
        inline void Disable() { m_is_enabled = false; }
        inline bool IsEnabled() const { return m_is_enabled; }

    protected:
        bool                   m_is_enabled;
        const std::string_view m_extension_name;
    };

    class VulkanInstanceExtension : public VulkanExtensionBase {
    public:
        VulkanInstanceExtension(std::string_view _ext_name) : VulkanExtensionBase(_ext_name) {}
        virtual ~VulkanInstanceExtension() = default;

        static TExtensionArray GetMERequiredInstanceExtensions();
    };

    class VulkanDeviceExtension : public VulkanExtensionBase {
    public:
        VulkanDeviceExtension(std::string_view _ext_name, bool _is_optional = false) : VulkanExtensionBase(_ext_name), m_is_optional(_is_optional), m_is_usable(true) {}
        virtual ~VulkanDeviceExtension() = default;

        static TVulkanDeviceExtensionArray GetMERequiredDeviceExtensions();
        static TVulkanDeviceExtensionArray GetMEEnabledDeviceExtensions(const Set<std::string>& _gpu_extensions);

        virtual bool IsOptional() const final { return m_is_optional; }
        virtual void PreGpuFeatures(VkPhysicalDeviceFeatures2& _gpu_features2) {}
        virtual void PostGpuFeatures(VulkanOptionalDeviceExtensions& _gpu_extensions) {}
        virtual void PreGpuProperties(const VulkanDevice* _device, VkPhysicalDeviceProperties2& _gpu_properties2) {}
        virtual void PreCreateDevice(VkDeviceCreateInfo& _device_create_info) {}

    protected:
        bool m_is_optional;
        bool m_is_usable;
    };

    class VulkanOptionalDeviceExtensions final {
    public:
        inline bool HasRaytracingExtensions() const {
            return m_has_khr_acceleration_structure &&
                   (m_has_khr_ray_tracing_pipeline || m_has_khr_ray_query);
        }

        // optional extensions
        bool m_has_ext_descriptor_buffer;
        bool m_has_khr_acceleration_structure;
        bool m_has_khr_ray_tracing_pipeline;
        bool m_has_khr_ray_query;

        bool m_has_memory_priority;
        bool m_has_pageable_device_local_memory;
        // nvidia
        bool m_has_nv_memory_decompression;
        bool m_has_nv_copy_memory_indirect;
    };
}// namespace Moer::Render

#endif//VULKAN_EXTENSION_H
