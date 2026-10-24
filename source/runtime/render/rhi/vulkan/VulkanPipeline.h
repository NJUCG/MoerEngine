#pragma once

#include "VulkanCommon.h"
#include "VulkanDescriptorMode.h"
#include "VulkanDeviceObject.h"
#include "VulkanPipelineArgument.h"
#include "rhi/RHIResource.h"

#include <optional>
#include <span>
#include <variant>

namespace Moer::Render {

struct VulkanDescriptorBindingInfo {
    VkDescriptorSetLayoutBinding vk_binding{};
    int                          argument_index = 0;
};
struct VulkanDescriptorSetLayoutCreateInfo {
    UnorderedMap<uint, VulkanDescriptorBindingInfo> bindings{};
    bool                                            is_bindless = false;
};

struct VulkanResourceBinding {
    uint             binding_index;
    uint             argument_index;
    VkDescriptorType descriptor_type;
    uint             descriptor_count;
    uint64           byte_offset      = 0;          // Binding offset relative to the set start.
    uint             heap_push_offset = UINT32_MAX; // Scalar buffer address or immutable sampler index.
};
struct VulkanResourceSetBindings {
    Array<VulkanResourceBinding> bindings;
    uint64                       allocation_size = 0; // Includes alignment and extra padding.
};

template<VkDescriptorType DescriptorType>
struct VulkanBindlessSetBindings {
    static_assert(
        DescriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
            DescriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
            DescriptorType == VK_DESCRIPTOR_TYPE_SAMPLER,
        "Unsupported bindless descriptor type"
    );
    static constexpr VkDescriptorType descriptor_type = DescriptorType;

    uint argument_index;   // Index in ArrayArguments.
    uint descriptor_count; // Count declared by the pipeline layout.
};
using VulkanBindlessBufferSetBindings  = VulkanBindlessSetBindings<VK_DESCRIPTOR_TYPE_STORAGE_BUFFER>;
using VulkanBindlessImageSetBindings   = VulkanBindlessSetBindings<VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE>;
using VulkanBindlessSamplerSetBindings = VulkanBindlessSetBindings<VK_DESCRIPTOR_TYPE_SAMPLER>;

using VulkanSetResourceBindings = std::variant<
    VulkanResourceSetBindings,
    VulkanBindlessBufferSetBindings,
    VulkanBindlessImageSetBindings,
    VulkanBindlessSamplerSetBindings>;

struct VulkanDescriptorSetBinding {
    uint buffer_index = 0;
    bool push             = false;
    uint heap_push_offset = 0;
    // Ordinary sets: relative to the draw's resource allocation.
    // Bindless sets: absolute offset in the bound descriptor buffer.
    uint64                    byte_offset = 0;
    VulkanSetResourceBindings resource_bindings;
};

struct VulkanBindlessIndexAddressBinding {
    uint argument_index;
    uint byte_offset;
};

struct VulkanPipelineBindingTemplate {
    // Ordinary resources, bindless buffers, and bindless images/samplers.
    static constexpr size_t descriptor_buffer_capacity = 3;

    VulkanDescriptorMode                          mode = VulkanDescriptorMode::Sets;
    Array<VkDescriptorSetAndBindingMappingEXT>    heap_mappings;
    VkShaderDescriptorSetAndBindingMappingInfoEXT heap_mapping_info{
        VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT
    };
    uint heap_push_size = 0;
    // Ascending order prevents lower-set rebinding from disturbing sets just bound above it.
    Map<uint, VulkanDescriptorSetBinding> set_bindings;
    // Native upload metadata for user bytes; stage visibility covers the whole block.
    VkPushConstantsInfoKHR push_constants_info{};
    // The user upload excludes this backend-owned 64-bit address.
    std::optional<VulkanBindlessIndexAddressBinding> bindless_index_address;
    uint64                                           resource_descriptor_allocation_size = 0;
    // Shared immutable metadata; bindless addresses are resolved per recording.
    Array<VkDescriptorBufferBindingInfoEXT> descriptor_buffers;
};

class VulkanPipelineState : public PipelineState, public VulkanDeviceObject {
    enum EType {
        GFX,
        Compute,
        RT
    };

public:
    VulkanPipelineState(VulkanDevice* _device, EType _type = EType::GFX, uint64 active_argument_bits = 0) :
        PipelineState(active_argument_bits),
        VulkanDeviceObject(_device),
        m_pipeline(VK_NULL_HANDLE),
        m_pipeline_layout(VK_NULL_HANDLE),
        m_type(_type) {};
    virtual ~VulkanPipelineState();

    inline VkPipeline GetHandle() const {
        return m_pipeline;
    }

    inline const VkPipelineLayout GetPipelineLayout() const {
        return m_pipeline_layout;
    }

    VulkanDescriptorMode GetDescriptorMode() const {
        return binding_template->mode;
    }
    VkDescriptorSetLayout GetDescriptorSetLayout(uint set) const {
        return descriptor_set_layouts.at(set);
    }

    bool UsesStencilAttachment() const {
        return b_uses_stencil_attachment;
    }

    uint32_t GetViewMask() const {
        return view_mask;
    }

    const VulkanArgumentInfo& GetArgumentInfo(uint argument_index) const {
        return m_argument_infos[argument_index];
    }

    std::span<const VulkanArgumentInfo> GetArgumentInfos() const {
        return m_argument_infos;
    }

    VkPipelineBindPoint GetPipelineBindPoint() const {
        switch (m_type) {
            case GFX:
                return VK_PIPELINE_BIND_POINT_GRAPHICS;
            case Compute:
                return VK_PIPELINE_BIND_POINT_COMPUTE;
            case RT:
                return VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR;
            default:
                return VK_PIPELINE_BIND_POINT_GRAPHICS;
        }
    }
    void InitPipelineLayout(
        UnorderedMap<uint, VulkanDescriptorSetLayoutCreateInfo>&& set_layout_infos,
        std::optional<VkPushConstantRange>                        push_constant_range,
        uint                                                      user_constant_byte_size,
        std::optional<VulkanBindlessIndexAddressBinding>          bindless_index_address
    );

public:
    UniquePtr<VulkanPipelineBindingTemplate> binding_template;

private:
    void CreatePipelineLayout(const std::optional<VkPushConstantRange>& push_constant_range);

protected:
    friend VulkanDevice;
    Array<VulkanArgumentInfo>    m_argument_infos;
    VkPipeline                   m_pipeline;
    VkPipelineLayout             m_pipeline_layout;
    Array<VkDescriptorSetLayout> descriptor_set_layouts;
    EType                        m_type;
    bool                         b_uses_stencil_attachment = false;
    uint32_t                     view_mask                 = 0;
};

} // namespace Moer::Render
