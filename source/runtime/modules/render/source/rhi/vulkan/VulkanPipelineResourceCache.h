#ifndef VULKAN_PIPELINE_STATE_CACHE_H
#define VULKAN_PIPELINE_STATE_CACHE_H

#include <map>
#include <array>
#include <vector>
#include <unordered_map>

#include <vulkan/vulkan.h>

class VulkanRHIGraphicsPipelineState;

class RHIBatchedShaderParameters;

struct DescriptorBindInfo {
    uint16_t               set;
    const VkDescriptorSet* descriptor_set;

    bool operator<(const DescriptorBindInfo& _info) const {
        return set < _info.set || *descriptor_set < *_info.descriptor_set;
    }
};

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
    friend VulkanRHIGraphicsPipelineState;

    VulkanPipelineResourceCache() : m_bound_descriptor_sets(), m_pushed_constants() {}

    void GetSetsToBind(std::vector<std::pair<uint32_t, const VkDescriptorSet*>>& _sets_to_bind);

    inline void AddSetToBind(const DescriptorBindInfo& _info) { m_bound_descriptor_sets[_info] = false; }

    inline void ResetToBind() { m_bound_descriptor_sets.clear(); }

    void GetConstantsToPush(std::vector<PushConstantInfo>& _constants_to_push);

    inline void AddConstantToPush(const PushConstantInfo& _info) { m_pushed_constants[_info] = false; }

    inline void ResetToPush() { m_pushed_constants.clear(); }

private:
    // <set, binding> -> descriptor set
    std::map<DescriptorBindInfo, bool> m_bound_descriptor_sets;

    // <start_addr, size> -> constant data
    std::map<PushConstantInfo, bool> m_pushed_constants;
};

#endif// VULKAN_PIPELINE_STATE_CACHE_H
