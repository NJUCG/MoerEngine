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
        if (layout) {
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
            case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
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
            resource_bindings.emplace<VulkanBindlessBufferBinding>(
                binding_info.argument_index, vk_binding.descriptorCount
            );
            break;
        case VK_DESCRIPTOR_TYPE_SAMPLER:
            resource_bindings.emplace<VulkanBindlessSamplerBinding>(
                binding_info.argument_index, vk_binding.descriptorCount
            );
            break;
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            resource_bindings.emplace<VulkanBindlessImageBinding>(
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
    for (const auto& [set_index, layout_info] : set_layout_infos) {
        auto& resource_bindings = binding_template.set_bindings[set_index].resource_bindings;

        if (layout_info.is_bindless)
            InitializeBindlessSetBindings(layout_info, resource_bindings);
        else
            InitializeNonBindlessSetBindings(layout_info, resource_bindings);
    }
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
    // Vulkan addresses sets by array index; gaps need layouts with this backend's flags.
    Array<VkDescriptorSetLayout> set_layouts(set_layout_count, VK_NULL_HANDLE);

    constexpr VkDescriptorBindingFlags bindless_array_binding_flags =
        VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
        VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;

    try {
        for (const auto& [set_index, layout_info] : set_layout_infos) {
            // FillMissingDescriptorBindings has made the binding indices contiguous.
            Array<VkDescriptorSetLayoutBinding> vk_bindings(layout_info.bindings.size());
            for (const auto& [binding_index, binding_info] : layout_info.bindings) {
                vk_bindings[binding_info.vk_binding.binding] = binding_info.vk_binding;
                if (layout_info.is_bindless)
                    vk_bindings[binding_info.vk_binding.binding].stageFlags = VK_SHADER_STAGE_ALL;
            }

            VkDescriptorSetLayoutCreateInfo layout_create_info{
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
            };
            layout_create_info.flags = binding_template.mode == VulkanDescriptorMode::Buffer ?
                                           VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT :
                                           0;
            if (binding_template.set_bindings.at(set_index).push)
                layout_create_info.flags |= VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
            if (layout_info.is_bindless && binding_template.mode == VulkanDescriptorMode::Sets)
                layout_create_info.flags |= VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
            layout_create_info.bindingCount = vk_bindings.size();
            layout_create_info.pBindings    = vk_bindings.data();

            VkDescriptorSetLayoutBindingFlagsCreateInfo binding_flags_info{
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO
            };
            const VkDescriptorBindingFlags flags =
                bindless_array_binding_flags | (binding_template.mode == VulkanDescriptorMode::Sets ?
                                                    VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT :
                                                    0);
            if (layout_info.is_bindless) {
                binding_flags_info.bindingCount  = 1;
                binding_flags_info.pBindingFlags = &flags;
                layout_create_info.pNext         = &binding_flags_info;
            }

            VK_CHECK_RESULT(vkCreateDescriptorSetLayout(
                device.GetDevice(), &layout_create_info, nullptr, &set_layouts[set_index]
            ));
            auto& set_binding = binding_template.set_bindings.at(set_index);
            if (auto* resource_bindings =
                    std::get_if<VulkanResourceSetBindings>(&set_binding.resource_bindings)) {
                if (binding_template.mode == VulkanDescriptorMode::Buffer && !set_binding.push)
                    CacheDescriptorSetStorageLayout(
                        device, set_layouts[set_index], layout_info, *resource_bindings
                    );
            }
        }
        for (auto& layout : set_layouts) {
            if (layout)
                continue;
            VkDescriptorSetLayoutCreateInfo empty{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            empty.flags = binding_template.mode == VulkanDescriptorMode::Buffer ?
                              VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT :
                              0;
            VK_CHECK_RESULT(vkCreateDescriptorSetLayout(device.GetDevice(), &empty, nullptr, &layout));
        }
    } catch (...) {
        for (VkDescriptorSetLayout layout : set_layouts) {
            if (layout) {
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
    constexpr uint invalid_buffer_index         = std::numeric_limits<uint>::max();
    uint           ordinary_buffer_index        = invalid_buffer_index;
    uint           bindless_buffer_index        = invalid_buffer_index;
    uint           bindless_image_sampler_index = invalid_buffer_index;

    auto& descriptor_buffers = binding_template.descriptor_buffers;
    descriptor_buffers.clear();
    auto allocate_buffer_index = [&](uint& buffer_index) {
        if (buffer_index == invalid_buffer_index) {
            buffer_index = static_cast<uint>(descriptor_buffers.size());
            descriptor_buffers.emplace_back();
        }
        return buffer_index;
    };

    binding_template.resource_descriptor_allocation_size = 0;
    for (auto& [set_index, set_binding] : binding_template.set_bindings) {
        if (set_binding.push)
            continue;
        // Ordinary sets share one buffer; bindless images and samplers share another.
        auto& group_index =
            std::holds_alternative<VulkanResourceSetBindings>(set_binding.resource_bindings) ?
                ordinary_buffer_index :
            std::holds_alternative<VulkanBindlessBufferBinding>(set_binding.resource_bindings) ?
                bindless_buffer_index :
                bindless_image_sampler_index;
        set_binding.buffer_index = allocate_buffer_index(group_index);
        auto& descriptor_buffer  = descriptor_buffers[set_binding.buffer_index];
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

bool IsHeapPushAddress(const VulkanResourceBinding& binding) {
    return binding.descriptor_count == 1 && (binding.descriptor_type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
                                             binding.descriptor_type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
}

void InitializeHeapBindings(VulkanDevice& device, VulkanPipelineBindingTemplate& result, uint push_size) {
    auto& storage       = device.GetGlobalDescriptorHeap();
    auto  allocate_push = [&](uint size) {
        push_size   = Moer::AlignUp(push_size, size);
        uint offset = push_size;
        push_size += size;
        return offset;
    };
    for (auto& [set, info] : result.set_bindings) {
        info.heap_push_offset = allocate_push(4);
        auto make_mapping     = [&](uint binding) -> VkDescriptorSetAndBindingMappingEXT& {
            auto& map                           = result.heap_mappings.emplace_back();
            map.sType                           = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT;
            map.descriptorSet                   = set;
            map.firstBinding                    = binding;
            map.bindingCount                    = 1;
            map.resourceMask                    = VK_SPIRV_RESOURCE_TYPE_ALL_EXT;
            map.source                          = VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_PUSH_INDEX_EXT;
            map.sourceData.pushIndex.pushOffset = info.heap_push_offset;
            map.sourceData.pushIndex.heapIndexStride = 1; // The pushed value is a byte offset.
            return map;
        };
        std::visit(
            Overload{
                [&](VulkanResourceSetBindings& resources) {
                    info.byte_offset = result.resource_descriptor_allocation_size;
                    for (auto& binding : resources.bindings) {
                        if (!binding.descriptor_count)
                            continue;
                        auto& map = make_mapping(binding.binding_index);
                        if (IsHeapPushAddress(binding) &&
                            !device.GetCoreFeatures().GetCore10Features().robustBufferAccess) {
                            binding.heap_push_offset         = allocate_push(8);
                            map.source                       = VK_DESCRIPTOR_MAPPING_SOURCE_PUSH_ADDRESS_EXT;
                            map.sourceData.pushAddressOffset = binding.heap_push_offset;
                        } else if (binding.descriptor_type == VK_DESCRIPTOR_TYPE_SAMPLER) {
                            binding.heap_push_offset            = allocate_push(4);
                            map.sourceData.pushIndex.pushOffset = binding.heap_push_offset;
                            map.sourceData.pushIndex.heapIndexStride =
                                storage.GetDescriptorSize(VK_DESCRIPTOR_TYPE_SAMPLER);
                            map.sourceData.pushIndex.heapArrayStride =
                                storage.GetDescriptorSize(VK_DESCRIPTOR_TYPE_SAMPLER);
                        } else {
                            resources.allocation_size =
                                Moer::AlignUp(resources.allocation_size, storage.GetStorageAlignment());
                            binding.byte_offset = resources.allocation_size;
                            const uint size     = storage.GetDescriptorSize(binding.descriptor_type);
                            map.sourceData.pushIndex.heapOffset      = static_cast<uint>(binding.byte_offset);
                            map.sourceData.pushIndex.heapArrayStride = size;
                            resources.allocation_size += uint64(size) * binding.descriptor_count;
                        }
                    }
                    resources.allocation_size =
                        Moer::AlignUp(resources.allocation_size, storage.GetStorageAlignment());
                    result.resource_descriptor_allocation_size += resources.allocation_size;
                },
                [&]<VkDescriptorType Type>(const VulkanBindlessBinding<Type>&) {
                    auto& map                                = make_mapping(0);
                    map.sourceData.pushIndex.heapArrayStride = storage.GetDescriptorSize(Type);
                    if constexpr (Type == VK_DESCRIPTOR_TYPE_SAMPLER) {
                        map.source = VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT;
                        map.sourceData.constantOffset                 = {};
                        map.sourceData.constantOffset.heapArrayStride = storage.GetDescriptorSize(Type);
                    }
                }
            },
            info.resource_bindings
        );
    }
    result.heap_push_size                 = push_size;
    result.heap_mapping_info.mappingCount = static_cast<uint>(result.heap_mappings.size());
    result.heap_mapping_info.pMappings    = result.heap_mappings.data();
}

std::optional<uint> SelectPushDescriptorSet(
    const UnorderedMap<uint, VulkanDescriptorSetLayoutCreateInfo>& set_layout_infos,
    VulkanDescriptorMode                                         mode,
    const VulkanDescriptorCapabilities&                          caps
) {
    if (!caps.push || mode == VulkanDescriptorMode::Heap ||
        (mode == VulkanDescriptorMode::Buffer && !caps.buffer_push)) {
        return std::nullopt;
    }

    // Only one ordinary set can use push descriptors. Prefer the largest eligible
    // set to avoid the most transient writes; break ties by the lower set index.
    std::optional<uint> candidate;
    uint                largest = 0;
    for (const auto& [set, layout] : set_layout_infos) {
        if (layout.is_bindless)
            continue;
        uint count = 0;
        for (const auto& [binding, info] : layout.bindings)
            count += info.vk_binding.descriptorCount;
        if (count && count <= caps.max_push_descriptors &&
            (!candidate || count > largest || (count == largest && set < *candidate))) {
            candidate = set;
            largest   = count;
        }
    }
    return candidate;
}

} // namespace

UniquePtr<VulkanPipelineBindingTemplate> VulkanPipelineState::BuildPipelineBindingTemplate(
    const UnorderedMap<uint, VulkanDescriptorSetLayoutCreateInfo>& set_layout_infos,
    const std::optional<VkPushConstantRange>&                      push_constant_range
) {
    auto&       device        = *m_device;
    const auto& caps          = device.GetDescriptorCapabilities();
    bool        requires_sets = false;

    for (const auto& [set, layout] : set_layout_infos) {
        for (const auto& [binding, info] : layout.bindings)
            requires_sets |= info.vk_binding.descriptorType == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
    }

    auto result  = MakeUnique<VulkanPipelineBindingTemplate>();
    result->mode = SelectVulkanPipelineDescriptorMode(device.GetDescriptorMode(), requires_sets);
    if (result->mode == VulkanDescriptorMode::Heap) {
        InitializePipelineSetBindings(set_layout_infos, *result);
        InitializeHeapBindings(
            device,
            *result,
            push_constant_range ? push_constant_range->offset + push_constant_range->size : 0
        );
        // Eligibility depends on the complete mapping, including appended push
        // data and alignment. Keep that mapping on success; reset it on fallback.
        if (result->heap_push_size <= caps.max_push_data)
            return result;
        *result = VulkanPipelineBindingTemplate{};
    }

    if (const auto push_set = SelectPushDescriptorSet(set_layout_infos, result->mode, caps))
        result->set_bindings[*push_set].push = true;
    InitializePipelineSetBindings(set_layout_infos, *result);

    // Sets and Buffer share native layouts. Buffer storage offsets are queried
    // during set-layout creation, then used to complete its binding template.
    descriptor_set_layouts = CreateDescriptorSetLayouts(device, set_layout_infos, *result);
    CreatePipelineLayout(push_constant_range);
    if (result->mode == VulkanDescriptorMode::Buffer)
        InitializeDescriptorBufferBindingTemplate(device, *result);
    return result;
}

void VulkanPipelineState::InitPipelineLayout(
    UnorderedMap<uint, VulkanDescriptorSetLayoutCreateInfo>&& set_layout_infos,
    std::optional<VkPushConstantRange>                        push_constant_range,
    uint                                                      user_constant_byte_size,
    std::optional<VulkanBindlessIndexAddressBinding>          bindless_index_address
) {
    binding_template = BuildPipelineBindingTemplate(set_layout_infos, push_constant_range);

    auto& result = *binding_template;
    InitializePushConstantBindingTemplate(m_pipeline_layout, push_constant_range, result);
    result.push_constants_info.size = user_constant_byte_size;
    result.bindless_index_address   = bindless_index_address;
}

} // namespace Moer::Render
