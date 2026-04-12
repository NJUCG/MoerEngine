/*
Vulkan extension 架构

1. 架构
- VulkanExtensionRegistry.cpp：维护所有 extension 描述表，是唯一登记入口。
- VulkanExtension.cpp：读取描述表，生成 instance/device extension 列表。
- VulkanExtensionFactories.cpp：放复杂 extension 的具体实现。
- 简单 extension 只写在 Registry 里；复杂 extension 再额外走 Factory。

2. 添加 extension
- 先判断它是 Instance 还是 Device，是 required 还是 optional。
- 如果是简单 extension：直接在 VulkanExtensionRegistry.cpp 加一条 descriptor。
- 如果是复杂 extension：
  在 VulkanExtensionFactories.cpp 里实现类和 factory，
  然后在 VulkanExtensionRegistry.cpp 里把 descriptor 指到这个 factory。
- 如果它会影响 capability 或 pNext/feature/property，记得在复杂 extension 实现里补齐对应逻辑。
*/

#ifndef VULKAN_EXTENSION_H
#define VULKAN_EXTENSION_H

#include <cstdint>
#include <string_view>

#include "../VulkanTypeDefs.h"

struct RHIInfo;
namespace Moer::Render {

class VulkanDevice;
class VulkanOptionalDeviceExtensions;
class VulkanDeviceExtension;

enum class EVulkanExtensionKind : uint8_t {
    Instance,
    Device
};

using TVulkanDeviceExtensionFactory = std::shared_ptr<VulkanDeviceExtension> (*)(bool);

struct VulkanExtensionDesc {
    EVulkanExtensionKind          kind     = EVulkanExtensionKind::Device;
    std::string_view              name     = {};
    bool                          optional = false;
    TVulkanDeviceExtensionFactory factory  = nullptr;
};

class VulkanExtensionBase {
public:
    VulkanExtensionBase(std::string_view _ext_name) : m_extension_name(_ext_name), m_is_enabled(false) {}
    virtual ~VulkanExtensionBase() = default;

    inline const std::string_view GetExtensionName() const {
        return m_extension_name;
    }

    inline void Enable() {
        m_is_enabled = true;
    }
    inline void Disable() {
        m_is_enabled = false;
    }
    inline bool IsEnabled() const {
        return m_is_enabled;
    }

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
    VulkanDeviceExtension(std::string_view _ext_name, bool _is_optional = false) :
        VulkanExtensionBase(_ext_name),
        m_is_optional(_is_optional),
        m_is_usable(true) {}
    virtual ~VulkanDeviceExtension() = default;

    static TVulkanDeviceExtensionArray GetMERequiredDeviceExtensions();
    static TVulkanDeviceExtensionArray GetMEEnabledDeviceExtensions(const Set<std::string>& _gpu_extensions);

    virtual bool IsOptional() const final {
        return m_is_optional;
    }
    virtual void PreGpuFeatures(VkPhysicalDeviceFeatures2& _gpu_features2) {}
    virtual void PostGpuFeatures(VulkanOptionalDeviceExtensions& _gpu_extensions) {}
    virtual void
    PreGpuProperties(const VulkanDevice* _device, VkPhysicalDeviceProperties2& _gpu_properties2) {}
    virtual void PreCreateDevice(VkDeviceCreateInfo& _device_create_info) {}

protected:
    bool m_is_optional;
    bool m_is_usable;
};

class VulkanOptionalDeviceExtensions final {
public:
    inline bool HasRaytracingExtensions() const {
        return m_has_khr_acceleration_structure && (m_has_khr_ray_tracing_pipeline || m_has_khr_ray_query);
    }

    // optional extensions
    bool m_has_ext_descriptor_buffer;
    bool m_has_khr_acceleration_structure;
    bool m_has_khr_ray_tracing_pipeline;
    bool m_has_khr_ray_query;
    bool m_has_ext_mesh_shader;
    bool m_allow_mesh_primitive_shading;

    bool m_has_memory_priority;
    bool m_has_pageable_device_local_memory;
    // nvidia
    bool m_has_nv_memory_decompression;
    bool m_has_nv_copy_memory_indirect;
};
} // namespace Moer::Render

#endif //VULKAN_EXTENSION_H
