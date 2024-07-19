//
// Created by 74535 on 2023/10/11.
//

#ifndef VULKAN_EXTENSION_H
#define VULKAN_EXTENSION_H

#include <string_view>
#define VULKAN_EXTENSION_ENABLED  1
#define VULKAN_EXTENSION_DISABLED 0
#define VULKAN_EXTENSION_OPTIONAL 1
#define VULKAN_EXTENSION_REQUIRED 0

#include "rhi/vulkan/misc/VulkanTypeDefs.h"
#include "log/LogSystem.h"

// struct RHIInfo;

// class VulkanDevice;
// class VulkanOptionalDeviceExtensions;

// class VulkanExtensionBase {
// public:
//     VulkanExtensionBase(const std::string& _ext_name, bool _is_enabled) : m_extension_name(_ext_name), m_is_enabled(_is_enabled) {}
//     virtual ~VulkanExtensionBase() = default;

//     inline const std::string& GetExtensionName() const { return m_extension_name; }

//     inline void Enable() { m_is_enabled = true; }
//     inline void Disable() { m_is_enabled = false; }
//     inline bool IsEnabled() const { return m_is_enabled; }

// protected:
//     bool              m_is_enabled;
//     const std::string m_extension_name;
// };

// class VulkanInstanceExtension : public VulkanExtensionBase {
// public:
//     VulkanInstanceExtension(const std::string& _ext_name, bool _is_enabled = true) : VulkanExtensionBase(_ext_name, _is_enabled) {}
//     virtual ~VulkanInstanceExtension() = default;

//     static TExtensionArray      GetMESupportedInstanceExtensions();
//     static TExtensionPropsArray GetDriverSupportedInstanceExtensions(const char* _layer_name = nullptr);
//     static TExtensionArray      GetDriverSupportedInstanceExtensionNames(const char* _layer_name = nullptr);
// };

// class VulkanDeviceExtension : public VulkanExtensionBase {
// public:
//     VulkanDeviceExtension(const std::string& _ext_name, bool _is_enabled = true, bool _is_optional = false) : VulkanExtensionBase(_ext_name, _is_enabled), m_is_optional(_is_optional), m_is_usable(true) {
//         LOG_INFO("VulkanDeviceExtension: {}, {}", _ext_name.c_str(), m_is_enabled);
//     }
//     virtual ~VulkanDeviceExtension() = default;

//     static TVulkanDeviceExtensionArray GetMESupportedDeviceExtensions(const RHIInfo& _rhi_info);
//     static TExtensionPropsArray        GetDriverSupportedDeviceExtensions(VkPhysicalDevice _gpu, const char* _layer_name = nullptr);
//     static TExtensionArray             GetDriverSupportedDeviceExtensionNames(VkPhysicalDevice _gpu, const char* _layer_name = nullptr);

//     virtual bool IsOptional() const final { return m_is_optional; }
//     virtual void PreGpuFeatures(VkPhysicalDeviceFeatures2& _gpu_features2) {}
//     virtual void PostGpuFeatures(VulkanOptionalDeviceExtensions& _gpu_extensions) {}
//     virtual void PreGpuProperties(const VulkanDevice* _device, VkPhysicalDeviceProperties2& _gpu_properties2) {}
//     virtual void PreCreateDevice(VkDeviceCreateInfo& _device_create_info) {}

// protected:
//     bool m_is_optional;
//     bool m_is_usable;
// };

// class VulkanOptionalDeviceExtensions final {
// public:
//     inline bool HasRaytracingExtensions() const {
//         return m_has_khr_acceleration_structure &&
//                (m_has_khr_ray_tracing_pipeline || m_has_khr_ray_query);
//     }

//     // optional extensions
//     bool m_has_ext_descriptor_buffer;
//     bool m_has_khr_acceleration_structure;
//     bool m_has_khr_ray_tracing_pipeline;
//     bool m_has_khr_ray_query;
// };

// class VulkanEnabledDeviceExtensions final {
// public:
//     void Init(const TVulkanDeviceExtensionArray& _enabled_extensions);

//     TVulkanDeviceExtensionArray m_enabled_extensions;
// };
struct RHIInfo;
namespace Moer::Render {

    class VulkanDevice;
    class VulkanOptionalDeviceExtensions;

    class VulkanExtensionBase {
    public:
        VulkanExtensionBase(std::string_view _ext_name, bool _is_enabled) : m_extension_name(_ext_name), m_is_enabled(_is_enabled) {}
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
        VulkanInstanceExtension(std::string_view _ext_name, bool _is_enabled = true) : VulkanExtensionBase(_ext_name, _is_enabled) {}
        virtual ~VulkanInstanceExtension() = default;

        static TExtensionArray      GetMESupportedInstanceExtensions();
        static TExtensionPropsArray GetDriverSupportedInstanceExtensions(const char* _layer_name = nullptr);
        static TExtensionArray      GetDriverSupportedInstanceExtensionNames(const char* _layer_name = nullptr);
    };
    using TVulkanDeviceExtensionArray = Moer::Array<std::shared_ptr<VulkanDeviceExtension>>;

    class VulkanDeviceExtension : public VulkanExtensionBase {
    public:
        VulkanDeviceExtension(std::string_view _ext_name, bool _is_enabled = true, bool _is_optional = false) : VulkanExtensionBase(_ext_name, _is_enabled), m_is_optional(_is_optional), m_is_usable(true) {
            LOG_INFO("VulkanDeviceExtension: {}, {}", _ext_name.data(), m_is_enabled);
        }
        virtual ~VulkanDeviceExtension() = default;

        static TVulkanDeviceExtensionArray GetMESupportedDeviceExtensions(const RHIInfo& _rhi_info);
        static TExtensionPropsArray        GetDriverSupportedDeviceExtensions(VkPhysicalDevice _gpu, const char* _layer_name = nullptr);
        static TExtensionArray             GetDriverSupportedDeviceExtensionNames(VkPhysicalDevice _gpu, const char* _layer_name = nullptr);

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
    };

    class VulkanEnabledDeviceExtensions final {
    public:
        void Init(const TVulkanDeviceExtensionArray& _enabled_extensions);

        TVulkanDeviceExtensionArray m_enabled_extensions;
    };
}// namespace Moer::Render

#endif//VULKAN_EXTENSION_H
