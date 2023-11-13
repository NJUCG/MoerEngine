#ifndef VULKAN_PIPELINE_STATE_CACHE_H
#define VULKAN_PIPELINE_STATE_CACHE_H

#include <set>
#include <array>
#include <vector>
#include <unordered_map>

#include <vulkan/vulkan.h>

class VulkanRHIGraphicsPipelineState;

class RHIBatchedShaderParameters;

struct DescriptorBindInfo {
    uint16_t        set;
    uint16_t        binding;
    VkDescriptorSet descriptor_set;

    bool operator<(const DescriptorBindInfo& _info) const {
        return set < _info.set || binding < _info.binding || descriptor_set < _info.descriptor_set;
    }
};

struct PushConstantInfo {
    VkShaderStageFlags   flags;
    uint32_t             size;
    uint32_t             byte_offset_in_raw_data;
    std::vector<uint8_t> raw_data;

    bool operator<(const PushConstantInfo& _info) const {
        return flags < _info.flags || size < _info.size || byte_offset_in_raw_data < _info.byte_offset_in_raw_data;
    }
};

class VulkanPipelineResourceCache {
public:
    friend VulkanRHIGraphicsPipelineState;

    VulkanPipelineResourceCache(VulkanRHIGraphicsPipelineState* _graphics_pso = nullptr) : m_pipeline_state(_graphics_pso) {}
    ~VulkanPipelineResourceCache() { m_pipeline_state = nullptr; }

    bool BindDescriptorSet(const DescriptorBindInfo& _info);

    inline const std::vector<DescriptorBindInfo>& GetSetsToBind() const { return m_sets_to_bind; }

    inline void AddSetToBind(const DescriptorBindInfo&& _info) { m_sets_to_bind.push_back(_info); }

    inline void ResetToBind() { m_sets_to_bind.clear(); }

    bool PushConstant(const PushConstantInfo& _info);

    inline const std::vector<PushConstantInfo>& GetConstantsToPush() const { return m_constant_to_push; }

    inline void AddConstantToPush(const PushConstantInfo&& _info) { m_constant_to_push.push_back(_info); }

    inline void ResetToPush() { m_constant_to_push.clear(); }

private:
    // <set, binding> -> descriptor set
    std::vector<DescriptorBindInfo> m_sets_to_bind;
    std::set<DescriptorBindInfo>    m_bound_descriptor_sets;

    // <start_addr, size> -> constant data
    std::vector<PushConstantInfo> m_constant_to_push;
    std::set<PushConstantInfo>    m_pushed_constants;

    VulkanRHIGraphicsPipelineState* m_pipeline_state;
};

#endif// VULKAN_PIPELINE_STATE_CACHE_H
