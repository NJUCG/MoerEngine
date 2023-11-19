#include "VulkanPipelineResourceCache.h"

#include "VulkanDevice.h"

#include "misc/Crc32.h"

void VulkanPipelineResourceCache::UpdateDescriptorSetHashInfo(uint32_t index, const VulkanHashableDescriptorInfo& _info) {
    m_descriptor_resource_container.hashable_descriptor_infos[index] = _info;
}

bool VulkanPipelineResourceCache::UpdateDescriptorSets(VulkanDevice* _device, const VulkanDescriptorSetsLayout* _layout) {
    return _device->GetDescriptorSets(GetSetsKey(), *_layout, m_descriptor_sets);
}

void VulkanPipelineResourceCache::BindDescriptorSets(VkCommandBuffer _buffer, VkPipelineBindPoint _bind_point, VkPipelineLayout _layout) {
    vkCmdBindDescriptorSets(
        _buffer,
        _bind_point,
        _layout,
        0,
        m_descriptor_sets.size(),
        m_descriptor_sets.data(),
        0,
        nullptr);
}

uint32_t VulkanPipelineResourceCache::GetSetsKey() const {
    return crc32_8bytes(
        m_descriptor_resource_container.hashable_descriptor_infos.data(),
        sizeof(VulkanHashableDescriptorInfo) * m_descriptor_resource_container.hashable_descriptor_infos.size());
}