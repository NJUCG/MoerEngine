#include "VulkanPipelineResourceCache.h"

#include "VulkanDevice.h"

#include "misc/Crc32.h"

void VulkanPipelineResourceCache::UpdateDescriptorSetHashInfos(uint32_t _index, const Moer::Array<VulkanHashableDescriptorInfo>& _infos) {
    for (const auto& info : _infos) {
        m_descriptor_resource_container.hashable_descriptor_infos[_index] = info;
        ++_index;
    }
}

const VkDescriptorImageInfo* VulkanPipelineResourceCache::UpdateDescriptorImageInfos(uint16_t _set, uint16_t _index_of_binding, const Moer::Array<VkDescriptorImageInfo>& _infos) {
    Moer::Array<VkDescriptorImageInfo> infos(_infos.size());
    for (uint16_t i = 0; i < _infos.size(); ++i) {
        m_descriptor_resource_container.descriptor_image_infos[_set][_index_of_binding + i] = _infos[i];
    }
    return m_descriptor_resource_container.descriptor_image_infos[_set].data() + _index_of_binding;
}

const VkDescriptorBufferInfo* VulkanPipelineResourceCache::UpdateDescriptorBufferInfos(uint16_t _set, uint16_t _index_of_binding, const Moer::Array<VkDescriptorBufferInfo>& _infos) {
    for (uint16_t i = 0; i < _infos.size(); ++i) {
        m_descriptor_resource_container.descriptor_buffer_infos[_set][_index_of_binding + i] = _infos[i];
    }
    return m_descriptor_resource_container.descriptor_buffer_infos[_set].data() + _index_of_binding;
}
#if VULKAN_RHI_RAYTRACING
const VkWriteDescriptorSetAccelerationStructureKHR& VulkanPipelineResourceCache::UpdateDescriptorASInfo(uint16_t _set, uint16_t _index_of_binding, const Moer::Array < VkWriteDescriptorSetAccelerationStructureKHR> & _infos) {
    for (uint16_t i = 0; i < _infos.size(); ++i) {
        m_descriptor_resource_container.descriptor_as_infos[_set][_index_of_binding + i] = _infos[i];
    }
    return m_descriptor_resource_container.descriptor_as_infos[_set].data() + _index_of_binding;
}
#endif


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