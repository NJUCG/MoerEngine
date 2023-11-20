#ifndef VULKAN_PIPELINE_STATE_CACHE_H
#define VULKAN_PIPELINE_STATE_CACHE_H

#include "VulkanDescriptor.h"

#include <map>
#include <vector>
#include <unordered_map>

class VulkanRHIGraphicsPipelineState;
class VulkanDescriptorSetsLayout;
class VulkanDevice;

struct PushConstantInfo {
    VkShaderStageFlags   flags;
    uint32_t             size;
    uint32_t             byte_offset_in_raw_data;
    std::vector<uint8_t> raw_data;
};

class VulkanPipelineResourceCache {
    friend VulkanDescriptorSetsLayout;
    friend VulkanDescriptorSetWriter;

public:
    VulkanPipelineResourceCache() = default;

    void UpdateDescriptorSetHashInfo(uint32_t _index, const VulkanHashableDescriptorInfo& _info);

    const VkDescriptorImageInfo& UpdateDescriptorImageInfo(uint16_t _set, uint16_t _index_of_binding, const VkDescriptorImageInfo& _info);

    const VkDescriptorBufferInfo& UpdateDescriptorBufferInfo(uint16_t _set, uint16_t _index_of_binding, const VkDescriptorBufferInfo& _info);

    bool UpdateDescriptorSets(VulkanDevice* _device, const VulkanDescriptorSetsLayout* _layout);

    void BindDescriptorSets(VkCommandBuffer _buffer, VkPipelineBindPoint _bind_point, VkPipelineLayout _layout);

    inline std::vector<VulkanDescriptorSetWriter>& GetWriters() { return m_descriptor_set_writers; }

    inline const std::vector<VkDescriptorSet>& GetDescriptorSets() const { return m_descriptor_sets; }

    inline const std::vector<PushConstantInfo>& GetConstantsToPush() const { return m_push_constants; }

    inline void AddConstantToPush(const PushConstantInfo& _info) { m_push_constants.push_back(_info); }

    inline void ResetToPush() { m_push_constants.clear(); }

private:
    VulkanDescriptorSetWriteContainer m_descriptor_resource_container;

    std::vector<VulkanDescriptorSetWriter> m_descriptor_set_writers;

    std::vector<VkDescriptorSet> m_descriptor_sets;

    std::vector<PushConstantInfo> m_push_constants;

    uint32_t GetSetsKey() const;
};

#endif// VULKAN_PIPELINE_STATE_CACHE_H
