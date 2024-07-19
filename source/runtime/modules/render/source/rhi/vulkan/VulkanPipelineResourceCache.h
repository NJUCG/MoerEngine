#ifndef VULKAN_PIPELINE_STATE_CACHE_H
#define VULKAN_PIPELINE_STATE_CACHE_H

#include "VulkanDescriptor.h"
#include "rhi/RHIResource.h"
namespace Moer::Render {
    class VulkanRHIGraphicsPipelineState;
    class VulkanDescriptorSetsLayout;
    class VulkanDevice;

    struct DescriptorSetBindingInfo {
        uint32_t         binding;
        VkDescriptorType type;
    };

    struct DescriptorSetInfo {
        Moer::Array<DescriptorSetBindingInfo> bindings;
        uint32_t                              image_count;
        uint32_t                              buffer_count;
        uint32_t                              as_count;
    };

    struct PushConstantInfo {
        VkShaderStageFlags   flags;
        uint32_t             size;
        uint32_t             byte_offset_in_raw_data;
        Moer::Array<uint8_t> raw_data;
    };

    class VulkanPipelineResourceCache {
        friend Moer::Render::VulkanDescriptorSetsLayout;
        friend Moer::Render::VulkanDescriptorSetWriter;

    public:
        VulkanPipelineResourceCache(const VulkanDescriptorSetsLayout* _layout, const Moer::Array<Moer::Render::TDescriptorSetLayoutBindingArray>& _descriptor_bindings);

        void SetSamplerState(uint32_t _set, uint32_t _binding, VulkanRHISampler* _sampler);

        void SetCBV(uint32_t _set, uint32_t _binding, RHICBV* _cbv);

        void SetSRV(uint32_t _set, uint32_t _binding, RHISRV* _srv);

        void SetUAV(uint32_t _set, uint32_t _binding, RHIUAV* _uav);

        void SetTexture(uint32_t _set, int32_t _binding, TextureView&& _view);
        void SetBuffer(uint32_t _set, int32_t _binding, BufferView&& _view);
        // void SetTlas()
        void PushConstant(uint _stage_flags, std::span<uint> _data);

        bool UpdateDescriptorSets(VulkanDevice* _device, const VulkanDescriptorSetsLayout* _layout);

        void BindDescriptorSets(VkCommandBuffer _buffer, VkPipelineBindPoint _bind_point, VkPipelineLayout _layout);

        inline bool HasDescriptorSets() const { return !m_descriptor_sets.empty(); }

        inline bool HasPushConstants() const { return !m_push_constants.empty(); }

        inline const Moer::Array<DescriptorSetInfo>& GetDescriptorSetInfos() const { return m_set_infos; }

        inline const Moer::Array<VkDescriptorSet>& GetDescriptorSets() const { return m_descriptor_sets; }

        inline const Moer::Array<PushConstantInfo>& GetConstantsToPush() const { return m_push_constants; }

        inline void AddConstantToPush(const PushConstantInfo& _info) { m_push_constants.push_back(_info); }
        inline void ResetToPush() { m_push_constants.clear(); }

    private:
        Moer::Render::VulkanDescriptorSetWriteContainer m_descriptor_resource_container;

        Moer::Array<Moer::Render::VulkanDescriptorSetWriter> m_descriptor_set_writers;

        Moer::Array<VkDescriptorSet> m_descriptor_sets;

        Moer::Array<PushConstantInfo> m_push_constants;

        uint32_t GetSetsKey() const;

        void InitDescriptorSetWriteContainer(const Moer::Array<Moer::Render::TDescriptorSetLayoutBindingArray>& _descriptor_bindings);

        Moer::Array<DescriptorSetInfo> m_set_infos;
    };
}// namespace Moer::Render
#endif// VULKAN_PIPELINE_STATE_CACHE_H
