#ifndef VULKAN_PIPELINE_STATE_CACHE_H
#define VULKAN_PIPELINE_STATE_CACHE_H

#include "VulkanDescriptor.h"

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

    bool operator<(const PushConstantInfo& _info) const {
        return flags < _info.flags || size < _info.size || byte_offset_in_raw_data < _info.byte_offset_in_raw_data || raw_data < _info.raw_data;
    }
};

class VulkanPipelineResourceCache {
public:
    friend VulkanDescriptorSetsLayout;

    VulkanPipelineResourceCache() : m_pushed_constants() {}

    void UpdateDescriptorSetHashInfo(uint32_t index, const VulkanHashableDescriptorInfo& _info);

    bool UpdateDescriptorSets(VulkanDevice* _device, const VulkanDescriptorSetsLayout* _layout);

    void BindDescriptorSets(VkCommandBuffer _buffer, VkPipelineBindPoint _bind_point, VkPipelineLayout _layout);

    inline std::vector<VulkanDescriptorSetWriter>& GetWriters() { return m_writers; }

    inline const std::vector<VkDescriptorSet>& GetDescriptorSets() const { return m_descriptor_sets; }

    inline std::unordered_map<PushConstantInfo, bool>& GetConstantsToPush() { return m_pushed_constants; }

    inline void AddConstantToPush(const PushConstantInfo& _info) { m_pushed_constants[_info] = false; }

    inline void ResetToPush() { m_pushed_constants.clear(); }

private:
    VulkanDescriptorSetWriteContainer m_descriptor_resource_container;

    std::vector<VulkanDescriptorSetWriter> m_writers;

    std::vector<VkDescriptorSet> m_descriptor_sets;

    // <start_addr, size> -> constant data
    std::unordered_map<PushConstantInfo, bool> m_pushed_constants;

    uint32_t GetSetsKey() const;
};

#endif// VULKAN_PIPELINE_STATE_CACHE_H
