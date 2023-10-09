//
// Created by 74535 on 2023/10/2.
//

#ifndef VULKAN_DEVICE_H
#define VULKAN_DEVICE_H

#include <vulkan.h>
#include <vk_mem_alloc.h>

#include <vector>
#include <map>

struct QueueFamilyIndices {
    std::optional<uint32_t> graphics;
    std::optional<uint32_t> present;
    std::optional<uint32_t> transfer;
    std::optional<uint32_t> compute;

    inline bool IsComplete() const { return graphics.has_value() && present.has_value() && transfer.has_value() && compute.has_value(); }
};

struct DeviceInitializer {
    VkInstance               instance           = nullptr;
    VkPhysicalDeviceType     gpu_type           = VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    VkSurfaceKHR             surface            = nullptr;
    VkPhysicalDeviceFeatures enabled_features   = {};
    std::vector<const char*> enabled_extensions = {};
    void*                    p_next_chain       = nullptr;
};

class VulkanDevice {
public:
    void Init(const DeviceInitializer& _initializer);
    void Destroy();

    inline VkPhysicalDevice GetGpu() const {
        return m_gpu;
    }
    inline VkDevice GetDevice() const {
        return m_device;
    }
    inline VkPhysicalDeviceProperties GetProperties() const {
        return m_gpu_props;
    }
    inline VkPhysicalDeviceFeatures GetFeatures() const {
        return m_gpu_features;
    }
    inline std::vector<const char*> GetGpuExtensions() const {
        return m_gpu_extensions;
    }
    inline VkPhysicalDeviceMemoryProperties GetMemoryProperties() const {
        return m_gpu_mem_props;
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
    inline VkCommandPool GetCommandPool() const {
        return m_command_pool;
    }

private:
    VkPhysicalDevice                 m_gpu;
    VkPhysicalDeviceProperties       m_gpu_props;
    VkPhysicalDeviceFeatures         m_gpu_features;
    VkPhysicalDeviceMemoryProperties m_gpu_mem_props;
    std::vector<const char*>         m_gpu_extensions;
    QueueFamilyIndices               m_queue_family_indices;

    VkDevice      m_device;
    VmaAllocator  m_allocator;
    VkQueue       m_graphics_queue;
    VkQueue       m_present_queue;
    VkQueue       m_compute_queue;
    VkQueue       m_transfer_queue;
    VkCommandPool m_command_pool;

private:
    VkPhysicalDevice SelectGpu(VkInstance _instance, VkPhysicalDeviceType _type, VkSurfaceKHR _surface, const std::vector<const char*>& _enabled_extensions);

    void CreateDevice(const DeviceInitializer& _initializer);

    std::vector<const char*>         GetGpuExtensions(VkPhysicalDevice _gpu) const;
    VkPhysicalDeviceMemoryProperties GetMemoryProperties(VkPhysicalDevice _gpu) const;
    //    uint32_t                         GetMemoryType(uint32_t type_bits, VkMemoryPropertyFlags properties, VkBool32* mem_type_found = nullptr) const;
    int32_t            GetQueueFamilyIndex(const std::vector<VkQueueFamilyProperties>& queue_family_props, VkQueueFlags _queue_flags) const;
    QueueFamilyIndices QueryQueueFamilyIndices(VkPhysicalDevice _gpu, VkSurfaceKHR _surface) const;

    bool CheckEnabledExtensionsSupported(VkPhysicalDevice _gpu, const std::vector<const char*>& _enabled_extensions) const;
};

#endif// VULKAN_DEVICE_H
