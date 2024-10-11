#include "VulkanPipelineResourceCache.h"

#include "VulkanDevice.h"
#include "VulkanUtil.h"
#include "VulkanMacroUtils.h"

#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "rhi/RHIResourceInitilizer.h"
namespace Moer::Render {
    VulkanPipelineResourceCache::VulkanPipelineResourceCache(const VulkanDescriptorSetsLayout* _layout, const Moer::Array<TDescriptorSetLayoutBindingArray>& _descriptor_bindings, VulkanDevice& _device) : m_device(_device) {
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
            auto& view_info = std::get<v_type_buffer_view>(_cbv->GetInfo().info);
            auto  range     = view_info.stride * view_info.num_elements;
            // ConstantBuffer
            m_descriptor_set_writers[_set].WriteUniformBuffer(_binding, buffer->GetHandle(), view_info.byte_offset, range);
        } else {
            // TextureBuffer
            LOG_CRITICAL("Texture CBV is not implemented yet.");
        }
    }

    void VulkanPipelineResourceCache::SetSRV(uint32_t _set, uint32_t _binding, RHISRV* _srv) {
        // MARK: buffer view is not implemented yet
        if (_srv->IsBuffer()) {
            auto* buffer    = static_cast<VulkanRHIBuffer*>(_srv->GetBuffer());
            auto& view_info = std::get<v_type_buffer_view>(_srv->GetInfo().info);
            auto  range     = view_info.stride * view_info.num_elements;
            m_descriptor_set_writers[_set].WriteStorageBuffer(_binding, buffer->GetHandle(), view_info.byte_offset, range);
        } else if (_srv->IsTexture()) {
            auto*          tex_view       = static_cast<VulkanRHITextureSRV*>(_srv);
            ETextureLayout default_layout = tex_view->GetTexture()->GetInfo().preferred_layout;
            const auto&    view_info      = std::get<v_type_texture_srv>(_srv->GetInfo().info);
            // MARK... this is not correct, need to re-implement view design
            auto min_mip      = view_info.mip_min;
            auto prev_usage   = tex_view->GetTexture()->mip_usages.find(Moer::uint(min_mip));
            auto final_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            if (prev_usage != tex_view->GetTexture()->mip_usages.end()) {
                auto usage   = std::get<ETextureStateFlags>(prev_usage->second);
                final_layout = EnumHasAnyFlag(usage, TS_UNORDERED_READ) ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            }

            m_descriptor_set_writers[_set].WriteSampledImage(_binding, VK_NULL_HANDLE, tex_view->GetView(), final_layout);
        } else {
            VulkanRHIRayTracingTLAS* as = static_cast<VulkanRHIRayTracingTLAS*>(_srv->GetAccelerationStructure());
            //MARK: accleration structure updated is not implemented,remaining updated_bit 0
            m_descriptor_set_writers[_set].WriteAccelerationStructure(_binding, as->m_tlas, 0);
        }
    }

    void VulkanPipelineResourceCache::SetUAV(uint32_t _set, uint32_t _binding, RHIUAV* _uav) {
        // MARK: buffer view is not implemented yet
        if (_uav->IsBuffer()) {
            auto* buffer    = static_cast<VulkanRHIBuffer*>(_uav->GetBuffer());
            auto& view_info = std::get<v_type_buffer_view>(_uav->GetInfo().info);
            auto  range     = view_info.stride * view_info.num_elements;
            m_descriptor_set_writers[_set].WriteStorageBuffer(_binding, buffer->GetHandle(), view_info.byte_offset, range);

        } else if (_uav->IsTexture()) {
            auto* tex_view = static_cast<VulkanRHITextureUAV*>(_uav);
            // MARK: layout is fixed
            m_descriptor_set_writers[_set].WriteStorageImage(_binding, tex_view->GetView(), VK_IMAGE_LAYOUT_GENERAL);
        } else {
            //MARK: accleration structure UAV is not implemented yet
        }
    }

    void VulkanPipelineResourceCache::SetTexture(uint32_t _set, int32_t _binding, TextureView&& _view) {
        auto*          image   = _view.texture;
        VulkanTexture* texture = reinterpret_cast<VulkanTexture*>(image);
        CHECK_ASSERT(texture != nullptr, "Texture is nullptr");

        VkImageLayout layout = texture->IsGeneralRead() ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
        m_descriptor_set_writers[_set].WriteImage(
            _binding,
            VK_NULL_HANDLE,
            texture->GetView(_view.mip_level, _view.num_mips),
            layout);
    }

    void VulkanPipelineResourceCache::SetBuffer(uint32_t _set, int32_t _binding, BufferView&& _view) {

        auto*         buffer        = _view.GetBuffer();
        VulkanBuffer* vulkan_buffer = reinterpret_cast<VulkanBuffer*>(buffer);
        CHECK_ASSERT(vulkan_buffer != nullptr, "Buffer is nullptr");

        m_descriptor_set_writers[_set].WriteBuffer(
            _binding,
            vulkan_buffer->GetHandle(),
            _view.GetByteOffset(),
            _view.GetByteSize());
    }

    void VulkanPipelineResourceCache::PushConstant(uint _stage_flags, std::span<uint> _data) {
        PushConstantInfo info;
        info.flags                   = _stage_flags;
        info.size                    = static_cast<uint32_t>(_data.size_bytes());
        info.byte_offset_in_raw_data = 0;
        info.raw_data                = Moer::Array<uint8_t>(_data.size_bytes());
        std::memcpy(info.raw_data.data(), _data.data(), _data.size_bytes());
        m_push_constants.push_back(info);
    }

    void VulkanPipelineResourceCache::SetSampler(uint _set, int32_t _binding, Sampler _sampler) {
        m_descriptor_set_writers[_set].WriteSampler(_binding, m_device.GetSampler(_sampler), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED);
    }

    bool VulkanPipelineResourceCache::UpdateDescriptorSets(const VulkanDescriptorSetsLayout* _layout) {
        if (m_descriptor_sets.empty()) {
            return false;
        }
        return true;
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
}// namespace Moer::Render