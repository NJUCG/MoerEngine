#include "VulkanPipelineResourceCache.h"

#include "VulkanDevice.h"

#include "misc/Crc32.h"

void VulkanPipelineResourceCache::UpdateDescriptorSetHashInfo(uint32_t _index, const VulkanHashableDescriptorInfo& _info) {
    m_descriptor_resource_container.hashable_descriptor_infos[_index] = _info;
}

const VkDescriptorImageInfo& VulkanPipelineResourceCache::UpdateDescriptorImageInfo(uint16_t _set, uint16_t _index_of_binding, const VkDescriptorImageInfo& _info) {
    m_descriptor_resource_container.descriptor_image_infos[_set][_index_of_binding] = _info;
    return m_descriptor_resource_container.descriptor_image_infos[_set][_index_of_binding];
}

const VkDescriptorBufferInfo& VulkanPipelineResourceCache::UpdateDescriptorBufferInfo(uint16_t _set, uint16_t _index_of_binding, const VkDescriptorBufferInfo& _info) {
    m_descriptor_resource_container.descriptor_buffer_infos[_set][_index_of_binding] = _info;
    return m_descriptor_resource_container.descriptor_buffer_infos[_set][_index_of_binding];
}

bool VulkanPipelineResourceCache::UpdateDescriptorSets(VulkanDevice* _device, const VulkanDescriptorSetsLayout* _layout) {
    if (m_descriptor_sets.empty()) {
        return false;
    }
    return _device->GetDescriptorSets(GetSetsKey(), *_layout, m_descriptor_set_writers, m_descriptor_sets);
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