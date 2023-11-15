#include "VulkanPipelineResourceCache.h"

void VulkanPipelineResourceCache::GetSetsToBind(std::vector<std::pair<uint32_t, const VkDescriptorSet*>>& _sets_to_bind) {
    for (auto& [info, bound] : m_bound_descriptor_sets) {
        if (!bound) {
            _sets_to_bind.emplace_back(info.set, info.descriptor_set);
        }
        bound = true;
    }
}

void VulkanPipelineResourceCache::GetConstantsToPush(std::vector<PushConstantInfo>& _constants_to_push) {
    for (auto& [info, pushed] : m_pushed_constants) {
        if (!pushed) {
            _constants_to_push.emplace_back(info);
        }
        pushed = true;
    }
}