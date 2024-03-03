#ifndef VULKAN_PIPELINE_STATE_CACHE_H
#define VULKAN_PIPELINE_STATE_CACHE_H

#include "VulkanDescriptor.h"

class VulkanRHIGraphicsPipelineState;
class VulkanDescriptorSetsLayout;
class VulkanDevice;

struct PushConstantInfo {
    VkShaderStageFlags   flags;
    uint32_t             size;
    uint32_t             byte_offset_in_raw_data;
    Moer::Array<uint8_t> raw_data;
};

class VulkanPipelineResourceCache {
    friend VulkanDescriptorSetsLayout;
    friend VulkanDescriptorSetWriter;

public:
    VulkanPipelineResourceCache() = default;

    void UpdateDescriptorSetHashInfos(uint32_t _index, const Moer::Array<VulkanHashableDescriptorInfo>& _infos);

    const VkDescriptorImageInfo* UpdateDescriptorImageInfos(uint16_t _set, uint16_t _index_of_binding, const Moer::Array<VkDescriptorImageInfo>& _infos);

    const VkDescriptorBufferInfo* UpdateDescriptorBufferInfos(uint16_t _set, uint16_t _index_of_binding, const Moer::Array<VkDescriptorBufferInfo>& _infos);

    const VkAccelerationStructureKHR* UpdateDescriptorASInfo(uint16_t _set, uint16_t _index_of_binding, const Moer::Array<VkAccelerationStructureKHR>& _infos);

    bool UpdateDescriptorSets(VulkanDevice* _device, const VulkanDescriptorSetsLayout* _layout);

    void BindDescriptorSets(VkCommandBuffer _buffer, VkPipelineBindPoint _bind_point, VkPipelineLayout _layout);

    inline bool HasDescriptorSets() const { return !m_descriptor_sets.empty(); }

    inline bool HasPushConstants() const { return !m_push_constants.empty(); }

    inline Moer::Array<VulkanDescriptorSetWriter>& GetWriters() { return m_descriptor_set_writers; }

    inline const Moer::Array<VkDescriptorSet>& GetDescriptorSets() const { return m_descriptor_sets; }

    inline const Moer::Array<PushConstantInfo>& GetConstantsToPush() const { return m_push_constants; }

    inline void AddConstantToPush(const PushConstantInfo& _info) { m_push_constants.push_back(_info); }

    inline void ResetToPush() { m_push_constants.clear(); }

private:
    VulkanDescriptorSetWriteContainer m_descriptor_resource_container;

    Moer::Array<VulkanDescriptorSetWriter> m_descriptor_set_writers;

    Moer::Array<VkDescriptorSet> m_descriptor_sets;

    Moer::Array<PushConstantInfo> m_push_constants;

    uint32_t GetSetsKey() const;
};

#endif// VULKAN_PIPELINE_STATE_CACHE_H
