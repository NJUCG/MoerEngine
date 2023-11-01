#ifndef VULKAN_RHI_DESCRIPTOR_H
#define VULKAN_RHI_DESCRIPTOR_H

#include "rhi/vulkan/misc/VulkanTypeDefs.h"

#include <vulkan/vulkan.h>

class VulkanDevice;

class VulkanDescriptorSetsLayout {
    friend class VulkanRHIGraphicsPipelineState;

public:
    VulkanDescriptorSetsLayout() {}
    ~VulkanDescriptorSetsLayout();

    void Init(VulkanDevice* device, const TDescriptorMap& _binding_count);

    inline uint32_t GetDescriptorSetCount() const {
        return m_layouts.size();
    }

    inline std::vector<VkDescriptorSetLayout> GetLayouts() const {
        return m_layouts;
    }

    inline const TDescriptorMap& GetBindingCount() const {
        return m_binding_count;
    }

private:
    std::vector<VkDescriptorSetLayout> m_layouts;

    TDescriptorMap m_binding_count;
};

class VulkanDescriptorAllocator {
public:
    VulkanDescriptorAllocator() : m_device(nullptr), m_current_pool(VK_NULL_HANDLE) {}
    ~VulkanDescriptorAllocator();

    void Init(VulkanDevice* device);

    bool Allocate(VkDescriptorSet& _set, const VulkanDescriptorSetsLayout& _layouts);

    void ResetAll();

    void CleanUp();

private:
    VulkanDevice* m_device;

    VkDescriptorPool m_current_pool;

    std::vector<VkDescriptorPool> m_used_pools;
    std::vector<VkDescriptorPool> m_free_pools;

private:
    VkDescriptorPool GetAvailablePool(const VulkanDescriptorSetsLayout& _layouts);

    VkDescriptorPool CreatePool(const VulkanDescriptorSetsLayout& _layout);

    uint32_t GetMaxSets(uint32_t _set_count) const;
};

#endif