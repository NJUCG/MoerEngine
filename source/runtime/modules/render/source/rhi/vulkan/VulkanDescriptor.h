#ifndef VULKAN_RHI_DESCRIPTOR_H
#define VULKAN_RHI_DESCRIPTOR_H

#include "rhi/vulkan/misc/VulkanTypeDefs.h"

#include <vulkan/vulkan.h>

class VulkanDevice;

class VulkanDescriptorSetsLayout {
    struct DescriptorBindingInfo {
        VkDescriptorType type;
        uint32_t         count;
    };
    using TDescriptorBindingInfo = std::unordered_map<uint8_t, std::unordered_map<uint32_t, DescriptorBindingInfo>>;

public:
    VulkanDescriptorSetsLayout() {}
    ~VulkanDescriptorSetsLayout();

    void Init(uint32_t _max_sets, const std::unordered_map<uint8_t, TDescriptorSetLayout>& _layout_mappings);

    inline uint32_t GetDescriptorSetCount() const {
        return m_layouts.size();
    }

    inline const std::vector<VkDescriptorSetLayout>& GetLayouts() const {
        return m_layouts;
    }

    inline const TDescriptorCountMap& GetSetsBindingCount() const {
        return m_sets_binding_count;
    }

    inline const TDescriptorBindingInfo& GetDescriptorBindingInfos() const {
        return m_descriptor_binding_infos;
    }

private:
    std::vector<VkDescriptorSetLayout> m_layouts;
    TDescriptorCountMap                m_sets_binding_count;
    TDescriptorBindingInfo             m_descriptor_binding_infos;
    // infos[space][slot] = {type, count}
};

class VulkanDescriptorAllocator {
public:
    VulkanDescriptorAllocator() : m_device(nullptr), m_current_pool(VK_NULL_HANDLE) {}
    ~VulkanDescriptorAllocator();

    void Init(VulkanDevice* device);

    bool Allocate(const VulkanDescriptorSetsLayout& _layouts, std::vector<VkDescriptorSet>& _sets);

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