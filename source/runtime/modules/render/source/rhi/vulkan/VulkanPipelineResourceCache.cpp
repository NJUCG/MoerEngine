#include "VulkanPipelineResourceCache.h"

#include "VulkanDevice.h"
#include "VulkanUtil.h"

#include "rhi/RHICommon.h"
#include "rhi/vulkan/misc/VulkanMacroUtils.h"
#include "vulkan/vulkan_core.h"

VulkanPipelineResourceCache::VulkanPipelineResourceCache(const VulkanDescriptorSetsLayout* _layout, const Moer::Array<TDescriptorSetLayoutBindingArray>& _descriptor_bindings) {
    const uint32_t set_count = _layout->GetDescriptorSetCount();
    const auto&    layouts   = _layout->GetLayouts();
    m_descriptor_set_writers.resize(set_count, this);
    m_descriptor_sets.resize(set_count, VK_NULL_HANDLE);

    InitDescriptorSetWriteContainer(_descriptor_bindings);

    auto* hash_info   = m_descriptor_resource_container.hashable_descriptor_info.data();
    auto* image_info  = m_descriptor_resource_container.descriptor_image_info.data();
    auto* buffer_info = m_descriptor_resource_container.descriptor_buffer_info.data();
    auto* as_info     = m_descriptor_resource_container.descriptor_as_info.data();

    auto* write_head    = m_descriptor_resource_container.descriptor_writes.data();
    auto* as_write_head = m_descriptor_resource_container.as_writes.data();

    // init descriptor write container data
    for (uint32_t set_idx = 0; set_idx < set_count; ++set_idx) {
        const auto& set_info = m_set_infos[set_idx];

        m_descriptor_set_writers[set_idx].Init(set_info.bindings, hash_info, write_head, image_info, buffer_info, as_write_head, as_info);

        hash_info += set_info.bindings.size();
        hash_info->layout = {UINT64_MAX, UINT64_MAX, layouts[set_idx]};
        ++hash_info;

        image_info += set_info.image_count;
        buffer_info += set_info.buffer_count;
        as_info += set_info.as_count;

        write_head += set_info.bindings.size();
        as_write_head += set_info.as_count;
    }
}

void VulkanPipelineResourceCache::SetSamplerState(uint32_t _set, uint32_t _binding, VulkanRHISampler* _sampler) {
    m_descriptor_set_writers[_set].WriteSampler(_binding, _sampler->GetHandle(), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED);
}

void VulkanPipelineResourceCache::SetCBV(uint32_t _set, uint32_t _binding, RHICBV* _cbv) {
    // MARK: offset should be set manually
    auto* buffer = static_cast<VulkanRHIBuffer*>(_cbv->GetBuffer());
    if (_cbv->IsBuffer()) {
        auto range = _cbv->GetInfo().buffer.cbv.stride * _cbv->GetInfo().buffer.cbv.num_elements;
        range      = std::min(uint64_t(range), buffer->GetInfo().size - _cbv->GetInfo().buffer.cbv.byte_offset);
        // ConstantBuffer
        m_descriptor_set_writers[_set].WriteUniformBuffer(_binding, buffer->GetHandle(), _cbv->GetInfo().buffer.cbv.byte_offset, range);
    } else {
        // TextureBuffer
        LOG_CRITICAL("Texture CBV is not implemented yet.");
    }
}

void VulkanPipelineResourceCache::SetSRV(uint32_t _set, uint32_t _binding, RHISRV* _srv) {
    // MARK: buffer view is not implemented yet
    if (_srv->IsBuffer()) {
        if (_srv->IsAccelerationStructure()) {
            // MARK: acceleration structure is not implemented yet
        } else {
            auto* buffer = static_cast<VulkanRHIBuffer*>(_srv->GetBuffer());
            m_descriptor_set_writers[_set].WriteStorageBuffer(_binding, buffer->GetHandle(), _srv->GetInfo().buffer.srv.byte_offset, buffer->GetInfo().size);
        }
    } else {
        auto* tex_view = static_cast<VulkanRHITextureSRV*>(_srv);
        // MARK: layout is fixed
        ETextureLayout default_layout = tex_view->GetTexture()->GetInfo().preferred_layout;
        auto           final_layout   = (default_layout == ETextureLayout::TEXTURE_LAYOUT_COMMON) ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        m_descriptor_set_writers[_set].WriteSampledImage(_binding, VK_NULL_HANDLE, tex_view->GetView(), final_layout);
    }
}

void VulkanPipelineResourceCache::SetUAV(uint32_t _set, uint32_t _binding, RHIUAV* _uav) {
    // MARK: buffer view is not implemented yet
    if (_uav->IsBuffer()) {
        if (_uav->IsAccelerationStructure()) {
            // MARK: acceleration structure is not implemented yet
        } else {
            auto* buffer = static_cast<VulkanRHIBuffer*>(_uav->GetBuffer());
            m_descriptor_set_writers[_set].WriteStorageBuffer(_binding, buffer->GetHandle(), _uav->GetInfo().buffer.uav.byte_offset, buffer->GetInfo().size);
        }
    } else {
        auto* tex_view = static_cast<VulkanRHITextureUAV*>(_uav);
        // MARK: layout is fixed
        m_descriptor_set_writers[_set].WriteStorageImage(_binding, tex_view->GetView(), VK_IMAGE_LAYOUT_GENERAL);
    }
}

bool VulkanPipelineResourceCache::UpdateDescriptorSets(VulkanDevice* _device, const VulkanDescriptorSetsLayout* _layout) {
    if (m_descriptor_sets.empty()) {
        return false;
    }
    return _device->GetDescriptorAllocator()->GetDescriptorSets(GetSetsKey(), *_layout, m_descriptor_set_writers, m_descriptor_sets);
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
    return Moer::RHI::Vulkan::Util::MemCrc32(
        m_descriptor_resource_container.hashable_descriptor_info.data(),
        sizeof(VulkanHashableDescriptorInfo) * m_descriptor_resource_container.hashable_descriptor_info.size());
}

void VulkanPipelineResourceCache::InitDescriptorSetWriteContainer(const Moer::Array<TDescriptorSetLayoutBindingArray>& _descriptor_bindings) {
    m_set_infos.resize(_descriptor_bindings.size(), {});

    auto& hash_info   = m_descriptor_resource_container.hashable_descriptor_info;
    auto& image_info  = m_descriptor_resource_container.descriptor_image_info;
    auto& buffer_info = m_descriptor_resource_container.descriptor_buffer_info;
    auto& as_info     = m_descriptor_resource_container.descriptor_as_info;
    auto& writes      = m_descriptor_resource_container.descriptor_writes;
    auto& as_writes   = m_descriptor_resource_container.as_writes;

    for (uint32_t set_idx = 0; set_idx < _descriptor_bindings.size(); ++set_idx) {
        for (const auto& binding : _descriptor_bindings[set_idx]) {
            m_set_infos[set_idx].bindings.push_back({binding.binding, binding.descriptorType});
            switch (binding.descriptorType) {
                case VK_DESCRIPTOR_TYPE_SAMPLER:
                case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
                case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
                case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
                case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
                    ++m_set_infos[set_idx].image_count;
                    break;
                case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
                case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                    ++m_set_infos[set_idx].buffer_count;
                    break;
                case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
                    ++m_set_infos[set_idx].as_count;
                    break;
                    // case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
                    // case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
                    //     LOG_ERROR("Unsupported descriptor type: {}", VK_TYPE_TO_STRING(VkDescriptorType, binding.descriptorType));
                    //     break;
                default:
                    LOG_ERROR("Unsupported descriptor type: {}", VK_TYPE_TO_STRING(VkDescriptorType, binding.descriptorType));
            }
        }
        hash_info.insert(hash_info.end(), 1 + m_set_infos[set_idx].bindings.size(), {});
        image_info.insert(image_info.end(), m_set_infos[set_idx].image_count, {});
        buffer_info.insert(buffer_info.end(), m_set_infos[set_idx].buffer_count, {});
        as_info.insert(as_info.end(), m_set_infos[set_idx].as_count, {});
        writes.insert(writes.end(), m_set_infos[set_idx].bindings.size(), {});
        as_writes.insert(as_writes.end(), m_set_infos[set_idx].as_count, {});
    }
}