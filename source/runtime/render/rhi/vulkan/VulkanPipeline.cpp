#include "VulkanPipeline.h"
#include "VulkanDescriptor.h"
#include "VulkanDevice.h"
#include "VulkanMacroUtils.h"
#include "log/LogSystem.h"
#include "misc/Alignment.h"

#include <algorithm>
#include <ranges>

namespace Moer::Render {

VulkanPipelineState::~VulkanPipelineState() {
    if (m_pipeline_layout) {
        vkDestroyPipelineLayout(m_device->GetDevice(), m_pipeline_layout, nullptr);
        m_pipeline_layout = VK_NULL_HANDLE;
    }
    if (m_pipeline) {
        vkDestroyPipeline(m_device->GetDevice(), m_pipeline, nullptr);
        m_pipeline = VK_NULL_HANDLE;
    }
    for (auto& layout : descriptor_set_layouts) {
        if (layout && layout != m_device->GetEmptyDescriptorSetLayout()) {
            vkDestroyDescriptorSetLayout(m_device->GetDevice(), layout, nullptr);
            layout = VK_NULL_HANDLE;
        }
    }
}

void VulkanPipelineState::CreatePipelineLayout(
    const std::optional<VkPushConstantRange>& push_constant_range
) {
    const bool has_push_constants = push_constant_range && push_constant_range->size != 0;

    VkPipelineLayoutCreateInfo pipeline_layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipeline_layout_info.setLayoutCount         = descriptor_set_layouts.size();
    pipeline_layout_info.pSetLayouts            = descriptor_set_layouts.data();
    pipeline_layout_info.pushConstantRangeCount = has_push_constants ? 1 : 0;
    pipeline_layout_info.pPushConstantRanges    = has_push_constants ? &*push_constant_range : nullptr;
    VK_CHECK_RESULT(
        vkCreatePipelineLayout(m_device->GetDevice(), &pipeline_layout_info, nullptr, &m_pipeline_layout)
    );
}

namespace {

void InitializeNonBindlessSetBindings(
    const VulkanDescriptorSetLayoutCreateInfo& layout_info,
    VulkanSetResourceBindings&                 resource_bindings
) {
    auto& ordinary_bindings = resource_bindings.emplace<VulkanResourceSetBindings>();
    ordinary_bindings.bindings.resize(layout_info.bindings.size());
    for (const auto& [binding_index, binding_info] : layout_info.bindings) {
        const auto& vk_binding            = binding_info.vk_binding;
        auto&       resource_binding      = ordinary_bindings.bindings[vk_binding.binding];
        resource_binding.binding_index    = vk_binding.binding;
        resource_binding.argument_index   = binding_info.argument_index;
        resource_binding.descriptor_type  = vk_binding.descriptorType;
        resource_binding.descriptor_count = vk_binding.descriptorCount;
        if (vk_binding.descriptorCount == 0) {
            continue;
        }

        switch (vk_binding.descriptorType) {
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
            case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            case VK_DESCRIPTOR_TYPE_SAMPLER:
            case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
                break;
            default:
                LOG_CRITICAL("Unsupported descriptor type: {}", uint(vk_binding.descriptorType));
                assert(false);
        }
    }
}

void InitializeBindlessSetBindings(
    const VulkanDescriptorSetLayoutCreateInfo& layout_info,
    VulkanSetResourceBindings&                 resource_bindings
) {
    const auto& binding_info = layout_info.bindings.at(0);
    const auto& vk_binding   = binding_info.vk_binding;
    switch (vk_binding.descriptorType) {
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            resource_bindings.emplace<VulkanBindlessBufferSetBindings>(
                binding_info.argument_index, vk_binding.descriptorCount
            );
            break;
        case VK_DESCRIPTOR_TYPE_SAMPLER:
            resource_bindings.emplace<VulkanBindlessSamplerSetBindings>(
                binding_info.argument_index, vk_binding.descriptorCount
            );
            break;
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            resource_bindings.emplace<VulkanBindlessImageSetBindings>(
                binding_info.argument_index, vk_binding.descriptorCount
            );
            break;
        default:
            LOG_CRITICAL("Unsupported bindless descriptor type: {}", uint(vk_binding.descriptorType));
            assert(false);
    }
}

void InitializePipelineSetBindings(
    const UnorderedMap<uint, VulkanDescriptorSetLayoutCreateInfo>& set_layout_infos,
    VulkanPipelineBindingTemplate&                                 binding_template
) {
    constexpr uint invalid_buffer_index         = std::numeric_limits<uint>::max();
    uint           bindless_buffer_index        = invalid_buffer_index;
    uint           bindless_image_sampler_index = invalid_buffer_index;
    uint           resource_buffer_index        = invalid_buffer_index;
    uint           descriptor_buffer_count      = 0;

    auto allocate_buffer_index = [&](uint& buffer_index) {
        if (buffer_index == invalid_buffer_index) {
            buffer_index = descriptor_buffer_count++;
        }
        return buffer_index;
    };

    binding_template.set_bindings.rehash(set_layout_infos.size());
    for (const auto& [set_index, layout_info] : set_layout_infos) {
        auto  binding_it        = binding_template.set_bindings.try_emplace(set_index).first;
        auto& set_binding       = binding_it->second;
        auto& resource_bindings = set_binding.resource_bindings;

        if (!layout_info.is_bindless) {
            InitializeNonBindlessSetBindings(layout_info, resource_bindings);
            set_binding.buffer_index = allocate_buffer_index(resource_buffer_index);
            continue;
        }

        InitializeBindlessSetBindings(layout_info, resource_bindings);
        // Bindless images and samplers share one descriptor buffer.
        auto& buffer_index = std::holds_alternative<VulkanBindlessBufferSetBindings>(resource_bindings) ?
                                 bindless_buffer_index :
                                 bindless_image_sampler_index;
        set_binding.buffer_index = allocate_buffer_index(buffer_index);
    }
    binding_template.descriptor_buffers.resize(descriptor_buffer_count);
}

void CacheDescriptorSetStorageLayout(
    const VulkanDevice&                        device,
    VkDescriptorSetLayout                      set_layout,
    const VulkanDescriptorSetLayoutCreateInfo& layout_info,
    VulkanResourceSetBindings&                 resource_bindings
) {
    for (const auto& [binding_index, binding_info] : layout_info.bindings) {
        const auto& vk_binding       = binding_info.vk_binding;
        auto&       resource_binding = resource_bindings.bindings[vk_binding.binding];
        vkGetDescriptorSetLayoutBindingOffsetEXT(
            device.GetDevice(), set_layout, vk_binding.binding, &resource_binding.byte_offset
        );
    }
    vkGetDescriptorSetLayoutSizeEXT(device.GetDevice(), set_layout, &resource_bindings.allocation_size);

    // Preserve the aligned size and extra padding used for AMD GPU compatibility.
    const uint64 alignment =
        device.GetOptionalProperties().descriptor_buffer_properties.descriptorBufferOffsetAlignment;
    resource_bindings.allocation_size =
        Moer::AlignUp(resource_bindings.allocation_size, alignment) + alignment;
}

Array<VkDescriptorSetLayout> CreateDescriptorSetLayouts(
    VulkanDevice&                                                  device,
    const UnorderedMap<uint, VulkanDescriptorSetLayoutCreateInfo>& set_layout_infos,
    VulkanPipelineBindingTemplate&                                 binding_template
) {
    const uint set_layout_count =
        set_layout_infos.empty() ? 0u : std::ranges::max(set_layout_infos | std::views::keys) + 1u;
    // Vulkan addresses sets by array index, so gaps require the shared empty layout.
    Array<VkDescriptorSetLayout> set_layouts(set_layout_count, device.GetEmptyDescriptorSetLayout());

    constexpr VkDescriptorBindingFlags bindless_array_binding_flags =
        VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
        VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;

    try {
        for (const auto& [set_index, layout_info] : set_layout_infos) {
            // FillMissingDescriptorBindings has made the binding indices contiguous.
            Array<VkDescriptorSetLayoutBinding> vk_bindings(layout_info.bindings.size());
            for (const auto& [binding_index, binding_info] : layout_info.bindings) {
                vk_bindings[binding_info.vk_binding.binding] = binding_info.vk_binding;
            }

            VkDescriptorSetLayoutCreateInfo layout_create_info{
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
            };
            layout_create_info.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
            layout_create_info.bindingCount = vk_bindings.size();
            layout_create_info.pBindings    = vk_bindings.data();

            VkDescriptorSetLayoutBindingFlagsCreateInfo binding_flags_info{
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO
            };
            if (layout_info.is_bindless) {
                binding_flags_info.bindingCount  = 1;
                binding_flags_info.pBindingFlags = &bindless_array_binding_flags;
                layout_create_info.pNext         = &binding_flags_info;
            }

            VK_CHECK_RESULT(vkCreateDescriptorSetLayout(
                device.GetDevice(), &layout_create_info, nullptr, &set_layouts[set_index]
            ));
            auto& set_binding = binding_template.set_bindings.at(set_index);
            if (auto* resource_bindings =
                    std::get_if<VulkanResourceSetBindings>(&set_binding.resource_bindings)) {
                CacheDescriptorSetStorageLayout(device, set_layouts[set_index], layout_info, *resource_bindings);
            }
        }
    } catch (...) {
        for (VkDescriptorSetLayout layout : set_layouts) {
            if (layout && layout != device.GetEmptyDescriptorSetLayout()) {
                vkDestroyDescriptorSetLayout(device.GetDevice(), layout, nullptr);
            }
        }
        throw;
    }
    return set_layouts;
}

void InitializeDescriptorBufferBindingTemplate(
    VulkanDevice&                  device,
    VulkanPipelineBindingTemplate& binding_template
) {
    binding_template.resource_descriptor_allocation_size = 0;
    for (auto& [set_index, set_binding] : binding_template.set_bindings) {
        auto& descriptor_buffer = binding_template.descriptor_buffers[set_binding.buffer_index];
        std::visit(
            [&](const auto& resource_bindings) {
                using ResourceBindings  = std::decay_t<decltype(resource_bindings)>;
                descriptor_buffer.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT;
                descriptor_buffer.usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                          VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT;

                if constexpr (std::is_same_v<ResourceBindings, VulkanResourceSetBindings>) {
                    set_binding.byte_offset = binding_template.resource_descriptor_allocation_size;
                    binding_template.resource_descriptor_allocation_size += resource_bindings.allocation_size;
                    descriptor_buffer.address = device.GetGlobalDescriptorHeap().GetDescriptorBufferAddress();
                    descriptor_buffer.usage |= VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT;
                } else {
                    // The bindless buffer address is supplied by each draw/dispatch.
                    descriptor_buffer.address = 0;
                    if constexpr (ResourceBindings::descriptor_type != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
                        descriptor_buffer.usage |= VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT;
                    }
                    if constexpr (ResourceBindings::descriptor_type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) {
                        set_binding.byte_offset = GetBindlessTextureDescriptorOffset(device);
                    }
                }
            },
            set_binding.resource_bindings
        );
    }
}

void InitializePushConstantBindingTemplate(
    VkPipelineLayout                          layout,
    const std::optional<VkPushConstantRange>& push_constant_range,
    VulkanPipelineBindingTemplate&            binding_template
) {
    if (!push_constant_range || push_constant_range->size == 0) {
        return;
    }

    VkPushConstantsInfoKHR& constants_info = binding_template.push_constants_info;

    constants_info.sType      = VK_STRUCTURE_TYPE_PUSH_CONSTANTS_INFO_KHR;
    constants_info.layout     = layout;
    constants_info.stageFlags = push_constant_range->stageFlags;
    constants_info.offset     = push_constant_range->offset;
    constants_info.size       = push_constant_range->size;
}

} // namespace

void VulkanPipelineState::InitPipelineLayout(
    UnorderedMap<uint, VulkanDescriptorSetLayoutCreateInfo>&& set_layout_infos,
    std::optional<VkPushConstantRange>                        push_constant_range,
    uint                                                      user_constant_byte_size,
    std::optional<VulkanBindlessIndexAddressBinding>          bindless_index_address
) {
    binding_template = MakeUnique<VulkanPipelineBindingTemplate>();

    InitializePipelineSetBindings(set_layout_infos, *binding_template);
    descriptor_set_layouts = CreateDescriptorSetLayouts(*m_device, set_layout_infos, *binding_template);

    CreatePipelineLayout(push_constant_range);

    InitializeDescriptorBufferBindingTemplate(*m_device, *binding_template);
    InitializePushConstantBindingTemplate(m_pipeline_layout, push_constant_range, *binding_template);
    binding_template->push_constants_info.size = user_constant_byte_size;
    binding_template->bindless_index_address   = bindless_index_address;
}

} // namespace Moer::Render
