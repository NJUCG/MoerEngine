#ifndef VULKAN_DEVICE_H
#define VULKAN_DEVICE_H

#include "misc/STL.h"
#include "rhi/vulkan/misc/VulkanTypeDefs.h"
#include "VulkanExtension.h"
#include "VulkanDeviceFeature.h"
#include "VulkanDeviceProperty.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <optional>

class VulkanDescriptorSetsLayout;
class VulkanDescriptorSetAllocator;
class VulkanDescriptorSetWriter;

struct QueueFamilyIndices {
    std::optional<uint32_t> graphics;
    std::optional<uint32_t> present;
    std::optional<uint32_t> transfer;
    std::optional<uint32_t> compute;
    std::optional<uint32_t> raytracing;

    inline bool IsComplete() const { return graphics.has_value() && present.has_value() && transfer.has_value() && compute.has_value() && raytracing.has_value(); }
};

struct DeviceInitializer {
    VkInstance                   instance = nullptr;
    VkPhysicalDeviceType         gpu_type = VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    VkSurfaceKHR                 surface;
    uint32_t                     api_version        = VK_API_VERSION_1_3;
    VulkanPhysicalDeviceFeatures enabled_features   = {};
    TVulkanDeviceExtensionArray  enabled_extensions = {};
};

class VulkanDevice {
public:
    VulkanDevice();

    void Init(const DeviceInitializer& _initializer);
    void InitMemoryAllocator(VkInstance _instance);
    void Destroy();

    operator VkDevice() const {
        return m_device;
    };

    inline VkPhysicalDevice GetGpu() const {
        return m_gpu;
    }
    inline VkDevice GetDevice() const {
        return m_device;
    }
    inline VmaAllocator GetVmaAllocator() const {
        return m_allocator;
    }
    inline const VulkanEnabledDeviceExtensions& GetEnabledExtensions() const {
        return m_enabled_extensions;
    }
    inline const VulkanOptionalDeviceExtensions& GetOptionalExtensions() const {
        return m_optional_extensions;
    }
    inline const VulkanPhysicalDeviceFeatures& GetCoreFeatures() const {
        return m_core_features;
    }
    inline const VulkanPhysicalDeviceProperties& GetCoreProperties() const {
        return m_core_properties;
    }
    inline const VulkanOptionalDeviceProperties& GetOptionalProperties() const {
        return m_optional_properties;
    }
    inline VkPhysicalDeviceMemoryProperties GetMemoryProperties() const {
        return m_memery_properties;
    }
    inline QueueFamilyIndices GetQueueFamilyIndices() const {
        return m_queue_family_indices;
    }
    inline VkQueue GetGraphicsQueue() const {
        return m_graphics_queue;
    }
    inline VkQueue GetPresentQueue() const {
        return m_present_queue;
    }
    inline VkQueue GetComputeQueue() const {
        return m_compute_queue;
    }
    inline VkQueue GetTransferQueue() const {
        return m_transfer_queue;
    }
    inline VkQueue GetRayTracingQueue() const {
        return m_raytracing_queue;
    }
    class VulkanCommandAllocator* GetCurrentCommandAllocator();
    bool                          GetDescriptorSets(uint32_t _hash_key, const VulkanDescriptorSetsLayout& _layout, Moer::Array<VulkanDescriptorSetWriter>& _writers, Moer::Array<VkDescriptorSet>& _sets);

private:
    VkPhysicalDevice                 m_gpu;
    VulkanEnabledDeviceExtensions    m_enabled_extensions;
    VulkanOptionalDeviceExtensions   m_optional_extensions;
    VulkanPhysicalDeviceFeatures     m_core_features;
    VulkanPhysicalDeviceProperties   m_core_properties;
    VulkanOptionalDeviceProperties   m_optional_properties;
    VkPhysicalDeviceMemoryProperties m_memery_properties;
    TQueueFamilyPropertiesArray      m_queue_family_props;
    QueueFamilyIndices               m_queue_family_indices;

    VkDevice m_device;
    VkQueue  m_graphics_queue;
    VkQueue  m_present_queue;
    VkQueue  m_compute_queue;
    VkQueue  m_raytracing_queue;
    VkQueue  m_transfer_queue;

    VmaAllocator                  m_allocator;
    VulkanDescriptorSetAllocator* m_descriptor_allocator;

    Moer::Array<class VulkanCommandAllocator*> m_command_allocators;

private:
    VkPhysicalDevice SelectGpu(const DeviceInitializer& _init);

    void InitGpu(const DeviceInitializer& _initializer);
    void CreateDevice(uint32_t _api_version);
    void CreateMemoryAllocator();
    void CreateDescriptorAllocator();
    void CreateCommandAllocators();

    TExtensionArray                  GetGpuExtensions(VkPhysicalDevice _gpu) const;
    VkPhysicalDeviceMemoryProperties GetMemoryProperties(VkPhysicalDevice _gpu) const;
    TQueueFamilyPropertiesArray      GetQueueFamilyProperties(VkPhysicalDevice _gpu) const;

    //    uint32_t                         GetMemoryType(uint32_t type_bits, VkMemoryPropertyFlags properties, VkBool32* mem_type_found = nullptr) const;
    int32_t            GetQueueFamilyIndex(const Moer::Array<VkQueueFamilyProperties>& queue_family_props, VkQueueFlags _queue_flags) const;
    QueueFamilyIndices QueryQueueFamilyIndices(VkPhysicalDevice _gpu, VkSurfaceKHR _surface) const;

    bool CheckEnabledExtensionsSupported(VkPhysicalDevice _gpu, const TVulkanDeviceExtensionArray& _enabled_extensions) const;
    bool CheckEnabledFeaturesSupported(VkPhysicalDevice _gpu, const VulkanPhysicalDeviceFeatures& _enabled_features, uint32_t _api_version);
};

#endif// VULKAN_DEVICE_H
