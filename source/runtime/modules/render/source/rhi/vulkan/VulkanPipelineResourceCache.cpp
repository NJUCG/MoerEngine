#include "VulkanPipelineResourceCache.h"

bool VulkanPipelineResourceCache::BindDescriptorSet(const DescriptorBindInfo& _info) {
    if (m_bound_descriptor_sets.contains(_info)) {
        return false;
    }
    m_bound_descriptor_sets.insert(_info);
    return true;
}

bool VulkanPipelineResourceCache::PushConstant(const PushConstantInfo& _info) {
    if (m_pushed_constants.contains(_info)) {
        return false;
    }
    m_pushed_constants.insert(_info);
    return true;
}