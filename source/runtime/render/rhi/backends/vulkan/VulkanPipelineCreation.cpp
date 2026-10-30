#include "VulkanDevice.h"
#include "VulkanEnumTranslator.h"
#include "VulkanMacroUtils.h"
#include "VulkanPipeline.h"
#include "VulkanVertexFormat.h"
#include "misc/RAII.h"
#include "misc/Types.h"
#include "rhi/RHIMultiview.h"
#include "rhi/ShaderConstantLayout.h"
#include "rhi/ShaderStageUtils.h"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace Moer::Render {

namespace {

VkDescriptorType METoVkDescriptorType(uint32 _desc_type) {
    EVulkanDescriptorType desc_type = EVulkanDescriptorType(_desc_type);
    switch (desc_type) {
        case VDT_UNIFORM_BUFFER:
            return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        case VDT_STORAGE_BUFFER:
            return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        case VDT_UNIFORM_TEXEL_BUFFER:
            return VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
        case VDT_STORAGE_TEXEL_BUFFER:
            return VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
        case VDT_SAMPLER:
            return VK_DESCRIPTOR_TYPE_SAMPLER;
        case VDT_SAMPLED_IMAGE:
            return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        case VDT_STORAGE_IMAGE:
            return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        case VDT_INPUT_ATTACHMENT:
            return VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
        case VDT_ACCELERATION_STRUCTURE:
            return VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        default:
            assert(false && "Invalid Descriptor Type.");
            break;
    }
    return VK_DESCRIPTOR_TYPE_MAX_ENUM;
}

struct VulkanPipelineBindingInfo {
    UnorderedMap<uint32, VulkanDescriptorSetLayoutCreateInfo> descriptor_set_layouts;
    VkPushConstantRange                                     push_constant_range{};
    uint32                                                    user_constant_byte_size = 0;
    std::optional<VulkanBindlessIndexAddressBinding>          bindless_index_address;
    Array<VulkanArgumentInfo>                               argument_infos;
    uint64                                                  active_argument_bits = 0;
};

VkPipelineStageFlags2 ToVulkanPipelineStageFlags(VkShaderStageFlagBits shader_stage) {
    switch (shader_stage) {

        case VK_SHADER_STAGE_VERTEX_BIT:
            return VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
        case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT:
            return VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT;
        case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT:
            return VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT;
        case VK_SHADER_STAGE_GEOMETRY_BIT:
            return VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT;
        case VK_SHADER_STAGE_FRAGMENT_BIT:
            return VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        case VK_SHADER_STAGE_COMPUTE_BIT:
            return VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        case VK_SHADER_STAGE_ALL_GRAPHICS:
            return VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;
        case VK_SHADER_STAGE_ALL:
            return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        case VK_SHADER_STAGE_RAYGEN_BIT_KHR:
            return VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
        case VK_SHADER_STAGE_ANY_HIT_BIT_KHR:
            return VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
        case VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR:
            return VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
        case VK_SHADER_STAGE_MISS_BIT_KHR:
            return VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
        case VK_SHADER_STAGE_INTERSECTION_BIT_KHR:
            return VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
        case VK_SHADER_STAGE_CALLABLE_BIT_KHR:
            return VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
        case VK_SHADER_STAGE_TASK_BIT_EXT:
            return VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_NV;
        case VK_SHADER_STAGE_MESH_BIT_EXT:
            return VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_NV;
        default:
            assert(false && "Invalid Shader Stage.");
    }
    return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
}

void MarkShaderArgumentActive(
    uint32                     argument_index,
    bool                       is_active,
    VulkanPipelineBindingInfo& out_binding_info
) {
    if (is_active) {
        out_binding_info.active_argument_bits |= uint64(1) << argument_index;
    }
}

void MergeDescriptorBinding(
    uint32                              set_index,
    uint32                              argument_index,
    const VkDescriptorSetLayoutBinding& incoming_binding,
    VulkanPipelineBindingInfo&          out_binding_info
) {
    auto& bindings         = out_binding_info.descriptor_set_layouts[set_index].bindings;
    auto [entry, inserted] = bindings.try_emplace(
        incoming_binding.binding,
        VulkanDescriptorBindingInfo{
            .vk_binding = incoming_binding, .argument_index = static_cast<int>(argument_index)
        }
    );
    if (inserted) {
        return;
    }

    auto& existing = entry->second;
    if (existing.argument_index != static_cast<int>(argument_index)) {
        throw std::invalid_argument(
            "Vulkan descriptor binding conflict at set " + std::to_string(set_index) + ", binding " +
            std::to_string(incoming_binding.binding) + ": C++ argument indices " +
            std::to_string(existing.argument_index) + " and " + std::to_string(argument_index) + " differ."
        );
    }
    auto& existing_binding = existing.vk_binding;
    if (existing_binding.descriptorType != incoming_binding.descriptorType ||
        existing_binding.descriptorCount != incoming_binding.descriptorCount) {
        throw std::invalid_argument(
            "Vulkan descriptor binding conflict at set " + std::to_string(set_index) + ", binding " +
            std::to_string(incoming_binding.binding) + ": descriptor descriptions differ."
        );
    }
    // Shared stages extend visibility while retaining the original descriptor and argument source.
    existing_binding.stageFlags |= incoming_binding.stageFlags;
}

void RecordResourceUsage(
    const ReflectParamInfo::Resource& resource,
    uint32                            argument_index,
    VkShaderStageFlagBits             shader_stage,
    VulkanPipelineBindingInfo&        out_binding_info
) {
    auto& info          = out_binding_info.argument_infos[argument_index];
    info.resource_type  = static_cast<EShaderResourceType>(resource.resource_type);
    info.image_sampling = static_cast<VulkanImageSampling>(resource.sampled);
    info.texel_format = resource.desc_type == VDT_UNIFORM_TEXEL_BUFFER || resource.desc_type == VDT_STORAGE_TEXEL_BUFFER ?
        resource.format : PF_UNDEFINED;
    info.stages |= ToVulkanPipelineStageFlags(shader_stage);
    MarkShaderArgumentActive(argument_index, resource.custom_flag.active, out_binding_info);
}

void AccumulateResourceBinding(
    const ReflectParamInfo::Resource& resource,
    const ShaderArgCppInfo&           cpp_argument_info,
    uint32                            argument_index,
    VkShaderStageFlagBits             shader_stage,
    VulkanPipelineBindingInfo&        out_binding_info
) {
    const VkDescriptorSetLayoutBinding binding{
        .binding         = resource.binding,
        .descriptorType  = METoVkDescriptorType(resource.desc_type),
        .descriptorCount = std::max(resource.count, cpp_argument_info.array_size),
        .stageFlags      = static_cast<VkShaderStageFlags>(shader_stage)
    };
    MergeDescriptorBinding(resource.set, argument_index, binding, out_binding_info);
    RecordResourceUsage(resource, argument_index, shader_stage, out_binding_info);
}

void RecordPushConstantUsage(
    const ReflectParamInfo::Constant& constant,
    uint32                            argument_index,
    VkShaderStageFlagBits             shader_stage,
    VulkanPipelineBindingInfo&        out_binding_info
) {
    if (!constant.custom_flag.active) {
        return;
    }
    out_binding_info.push_constant_range.stageFlags |= shader_stage;
    MarkShaderArgumentActive(argument_index, constant.custom_flag.active, out_binding_info);
}

void AccumulateBindlessBindings(
    const ReflectParamInfo::BindlessArray& resources,
    uint32                                 argument_index,
    VkShaderStageFlagBits                  shader_stage,
    VulkanPipelineBindingInfo&             out_binding_info
) {
    out_binding_info.argument_infos[argument_index].stages |= ToVulkanPipelineStageFlags(shader_stage);
    if (resources.array) {
        throw std::invalid_argument("Vulkan bindless shaders must read the index table by device address");
    }
    if (resources.index_table_address && resources.index_table_address->custom_flag.active) {
        const auto& address = *resources.index_table_address;
        if (address.size != sizeof(VkDeviceAddress) || address.offset % alignof(VkDeviceAddress) != 0) {
            throw std::invalid_argument("Invalid Vulkan bindless index-table address layout");
        }
        if (out_binding_info.bindless_index_address &&
            (out_binding_info.bindless_index_address->argument_index != argument_index ||
             out_binding_info.bindless_index_address->byte_offset != address.offset)) {
            throw std::invalid_argument("Shader stages disagree on the bindless index-table address");
        }
        out_binding_info.bindless_index_address = {argument_index, address.offset};
        out_binding_info.push_constant_range.stageFlags |= shader_stage;
        MarkShaderArgumentActive(argument_index, true, out_binding_info);
    }

    constexpr uint32 k_resource_descriptor_count = 5000;
    constexpr uint32 k_sampler_descriptor_count  = 256;

    const auto add_table_binding = [&](const ReflectParamInfo::Bindless& table,
                                       uint32                            set_index,
                                       uint32                            binding_index,
                                       VkDescriptorType                  descriptor_type,
                                       uint32                            descriptor_count) {
        if (table.binding != binding_index) {
            throw std::invalid_argument("Vulkan bindless descriptor arrays must use binding 0");
        }
        MergeDescriptorBinding(
            set_index,
            argument_index,
            {.binding         = binding_index,
             .descriptorType  = descriptor_type,
             .descriptorCount = descriptor_count,
             .stageFlags      = static_cast<VkShaderStageFlags>(shader_stage)},
            out_binding_info
        );
        out_binding_info.descriptor_set_layouts.at(set_index).is_bindless = true;
        MarkShaderArgumentActive(argument_index, table.custom_flag.active, out_binding_info);
    };

    if (resources.buffer) {
        add_table_binding(
            *resources.buffer,
            resources.buffer->set,
            0,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            k_resource_descriptor_count
        );
    }
    if (resources.image) {
        add_table_binding(
            *resources.image,
            resources.image->set,
            0,
            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            k_resource_descriptor_count
        );
    }
    if (resources.sampler) {
        add_table_binding(
            *resources.sampler,
            resources.sampler->set,
            0,
            VK_DESCRIPTOR_TYPE_SAMPLER,
            k_sampler_descriptor_count
        );
    }
}

// Match this shader's reflection to C++ arguments and accumulate the pipeline's layout and binding metadata.
void AccumulateShaderPipelineBindings(
    const SingleShaderInfo&               shader,
    std::span<const PipelineArgumentInfo> arguments,
    VkShaderStageFlagBits                 shader_stage,
    VulkanPipelineBindingInfo&            out_binding_info
) {
    const auto& reflection_map = shader.shader_param_map->reflect_map;
    for (uint32 argument_index = 0; argument_index < arguments.size(); ++argument_index) {
        const auto& argument = arguments[argument_index];
        const auto  reflection_name =
            argument.cpp_info.type == SDA_BindlessArray ? ReflectParamInfo::bdls_name : argument.name;
        const auto found = reflection_map.find(std::string(reflection_name));
        if (found == reflection_map.end()) {
            continue;
        }

        const auto& reflection = found->second.GetSpirvReflection();
        switch (argument.cpp_info.type) {
            case SDA_BindlessArray:
                AccumulateBindlessBindings(
                    reflection.Get<ReflectParamInfo::BindlessArray>(argument.name, "BindlessArray"),
                    argument_index,
                    shader_stage,
                    out_binding_info
                );
                break;
            case SDA_Constant:
                RecordPushConstantUsage(
                    reflection.Get<ReflectParamInfo::Constant>(argument.name, "Constant"),
                    argument_index,
                    shader_stage,
                    out_binding_info
                );
                break;
            case SDA_Buffer:
            case SDA_Texture:
            case SDA_TLAS:
            case SDA_Sampler:
                AccumulateResourceBinding(
                    reflection.Get<ReflectParamInfo::Resource>(argument.name, "Resource"),
                    argument.cpp_info,
                    argument_index,
                    shader_stage,
                    out_binding_info
                );
                break;
            default:
                assert(false && "Unknown shader arg type.");
        }
    }
}

VulkanPipelineBindingInfo BuildVulkanPipelineBindingInfo(
    std::span<const SingleShaderInfo* const> shaders,
    const PipelineShaderInfo&                shader_info,
    uint32                                   max_push_constant_size
) {
    VulkanPipelineBindingInfo binding_info;
    const auto arguments = shader_info.argument_metadata.GetArguments();
    binding_info.user_constant_byte_size = ValidatePipelineConstants(shader_info, max_push_constant_size);
    binding_info.argument_infos.resize(arguments.size());
    for (const auto* shader : shaders) {
        const auto shader_stage = static_cast<VkShaderStageFlagBits>(
            VulkanEnumTranslator::METoVKShaderStageFlags(shader->shader_type)
        );
        AccumulateShaderPipelineBindings(*shader, arguments, shader_stage, binding_info);
        for (const auto& [name, parameter] : shader->shader_param_map->reflect_map) {
            const auto& reflection = parameter.GetSpirvReflection();
            const auto* constant   = reflection.TryGetConstant();
            if (const auto* bindless = reflection.TryGetBindlessArray();
                bindless && bindless->index_table_address)
                constant = &*bindless->index_table_address;
            if (constant) {
                binding_info.push_constant_range.size =
                    std::max(binding_info.push_constant_range.size, constant->offset + constant->size);
                binding_info.push_constant_range.stageFlags |= shader_stage;
            }
        }
    }
    binding_info.push_constant_range.size =
        std::max(binding_info.push_constant_range.size, binding_info.user_constant_byte_size);
    if (binding_info.bindless_index_address) {
        binding_info.push_constant_range.size =
            std::max(binding_info.push_constant_range.size, uint32(binding_info.bindless_index_address->byte_offset + sizeof(VkDeviceAddress)));
    }
    if (binding_info.push_constant_range.size > max_push_constant_size)
        throw std::invalid_argument("Declared push constant block exceeds the device limit");
    return binding_info;
}

Array<VkPipelineShaderStageCreateInfo>
CreateVulkanShaderStages(VkDevice device, std::span<const SingleShaderInfo* const> shaders) {
    Array<VkPipelineShaderStageCreateInfo> stages;
    stages.reserve(shaders.size());
    try {
        for (const auto* shader : shaders) {
            VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            module_info.codeSize = shader->shader_data.size();
            module_info.pCode    = reinterpret_cast<const uint32*>(shader->shader_data.data());
            VkShaderModule module;
            VK_CHECK_RESULT(vkCreateShaderModule(device, &module_info, nullptr, &module));
            VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            stage.stage = static_cast<VkShaderStageFlagBits>(
                VulkanEnumTranslator::METoVKShaderStageFlags(shader->shader_type)
            );
            stage.module = module;
            stage.pName  = shader->entry_point.data();
            stages.push_back(stage);
        }
    } catch (...) {
        for (const auto& stage : stages) {
            vkDestroyShaderModule(device, stage.module, nullptr);
        }
        throw;
    }
    return stages;
}

void FillMissingDescriptorBindings(UnorderedMap<uint32, VulkanDescriptorSetLayoutCreateInfo>& out_set_layouts) {
    for (auto& [set_id, set_layout] : out_set_layouts) {
        uint32 max_binding = 0;
        for (const auto& [binding_id, binding] : set_layout.bindings) {
            max_binding = std::max(max_binding, binding_id);
        }
        for (uint32 binding_id = 0; binding_id <= max_binding; ++binding_id) {
            set_layout.bindings.try_emplace(
                binding_id,
                VulkanDescriptorBindingInfo{
                    .vk_binding = {.binding = binding_id, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER}
                }
            );
        }
    }
}

void InitializeVulkanPipelineLayout(VulkanPipelineState& pipeline, VulkanPipelineBindingInfo& binding_info) {
    FillMissingDescriptorBindings(binding_info.descriptor_set_layouts);
    pipeline.InitPipelineLayout(
        std::move(binding_info.descriptor_set_layouts), binding_info.push_constant_range,
        binding_info.user_constant_byte_size, binding_info.bindless_index_address
    );
}

bool ValidateGraphicsPipelineCreateInfo(
    const VulkanDevice&       device,
    const VulkanDeviceInfo&   device_info,
    const GfxPsoCreateInfo&   create_info,
    const PipelineShaderInfo& shader_info
) {
    if (const auto error = ValidateGraphicsShaderStages(shader_info.shaders); !error.empty()) {
        LOG_ERROR("Cannot create Vulkan graphics pipeline: {}", error);
        return false;
    }
    const uint32 required_view_count = Multiview::RequiredViewCount(create_info.view_mask);
    if (create_info.view_mask != 0 && !device.SupportsMultiview(required_view_count)) {
        LOG_ERROR(
            "Cannot create multiview graphics pipeline: view_mask=0x{:x} requires {} views, "
            "but the selected device supports at most {} (feature enabled={}).",
            create_info.view_mask,
            required_view_count,
            device_info.core_properties.core_1_1.maxMultiviewViewCount,
            device_info.core_features.GetCore11Features().multiview == VK_TRUE
        );
        return false;
    }

    const bool uses_tessellation = FindShaderStage(shader_info.shaders, ST_HULL) != nullptr;
    if (uses_tessellation) {
        const uint32 patch_control_points = create_info.patch_control_points;
        const uint32 max_patch_size       = device_info.core_properties.core_1_0.limits.maxTessellationPatchSize;
        if (!device.SupportsTessellation()) {
            LOG_ERROR("Cannot create tessellation pipeline: the Vulkan device does not support tessellation "
                      "shaders.");
            return false;
        }
        if (create_info.primitive_topology != EPrimitiveTopology::PATCH_LIST) {
            LOG_ERROR("Cannot create tessellation pipeline: primitive topology must be PATCH_LIST.");
            return false;
        }
        if (patch_control_points == 0 || patch_control_points > max_patch_size) {
            LOG_ERROR(
                "Cannot create tessellation pipeline: patch control point count {} is outside [1, {}].",
                patch_control_points,
                max_patch_size
            );
            return false;
        }
    } else if (create_info.primitive_topology == EPrimitiveTopology::PATCH_LIST) {
        LOG_ERROR("Cannot create PATCH_LIST pipeline without Hull and Domain shaders.");
        return false;
    }
    return true;
}

VkPipelineColorBlendAttachmentState ToVulkanBlendAttachment(const RHIBlendAttachmentInfo& info) {
    VkPipelineColorBlendAttachmentState state{};
    state.blendEnable         = (info.color_blend_op != BO_ADD || info.color_dst_blend_factor != BF_ZERO ||
                         info.color_src_blend_factor != BF_ONE || info.alpha_blend_op != BO_ADD ||
                         info.alpha_dst_blend_factor != BF_ZERO || info.alpha_src_blend_factor != BF_ONE) ?
                                    VK_TRUE :
                                    VK_FALSE;
    state.srcColorBlendFactor = VulkanEnumTranslator::METoVKBlendFactor(info.color_src_blend_factor);
    state.dstColorBlendFactor = VulkanEnumTranslator::METoVKBlendFactor(info.color_dst_blend_factor);
    state.colorBlendOp        = VulkanEnumTranslator::METoVKBlendOp(info.color_blend_op);
    state.srcAlphaBlendFactor = VulkanEnumTranslator::METoVKBlendFactor(info.alpha_src_blend_factor);
    state.dstAlphaBlendFactor = VulkanEnumTranslator::METoVKBlendFactor(info.alpha_dst_blend_factor);
    state.alphaBlendOp        = VulkanEnumTranslator::METoVKBlendOp(info.alpha_blend_op);
    state.colorWriteMask      = (info.color_write_mask & CW_RED) ? VK_COLOR_COMPONENT_R_BIT : 0;
    state.colorWriteMask |= (info.color_write_mask & CW_GREEN) ? VK_COLOR_COMPONENT_G_BIT : 0;
    state.colorWriteMask |= (info.color_write_mask & CW_BLUE) ? VK_COLOR_COMPONENT_B_BIT : 0;
    state.colorWriteMask |= (info.color_write_mask & CW_ALPHA) ? VK_COLOR_COMPONENT_A_BIT : 0;
    return state;
}

VkPipelineRasterizationStateCreateInfo ToVulkanRasterizationState(const RHIRasterizeInfo& info) {
    VkPipelineRasterizationStateCreateInfo state{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    state.depthClampEnable        = info.b_depth_clamp_enable ? VK_TRUE : VK_FALSE;
    state.rasterizerDiscardEnable = VK_FALSE;
    state.polygonMode             = VulkanEnumTranslator::METoVKPolygonMode(info.fill_mode);
    state.cullMode                = VulkanEnumTranslator::METoVKCullModeFlags(info.cull_mode);
    state.frontFace =
        info.b_front_counter_clockwise ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    state.depthBiasEnable         = info.b_depth_bias ? VK_TRUE : VK_FALSE;
    state.depthBiasConstantFactor = info.depth_bias;
    state.depthBiasClamp          = info.depth_bias_clamp;
    state.depthBiasSlopeFactor    = info.depth_bias_slop_factor;
    state.lineWidth               = 1.0f;
    return state;
}

VkPipelineMultisampleStateCreateInfo ToVulkanMultisampleState(const RHIMultisampleStateInfo& info) {
    VkPipelineMultisampleStateCreateInfo state{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    state.rasterizationSamples  = VulkanEnumTranslator::METoVKSampleCountFlagBits(info.sample_count);
    state.sampleShadingEnable   = VK_FALSE;
    state.minSampleShading      = 1.0f;
    state.pSampleMask           = nullptr;
    state.alphaToCoverageEnable = VK_FALSE;
    state.alphaToOneEnable      = VK_FALSE;
    return state;
}

VkPipelineDepthStencilStateCreateInfo ToVulkanDepthStencilState(const RHIDepthStencilStateInfo& info) {
    VkPipelineDepthStencilStateCreateInfo state{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    state.depthTestEnable =
        (info.b_enable_depth_write || info.depth_test_op != ECompareOption::CO_NEVER) ? VK_TRUE : VK_FALSE;
    state.depthWriteEnable      = info.b_enable_depth_write;
    state.depthCompareOp        = VulkanEnumTranslator::METoVKCompareOp(info.depth_test_op);
    state.depthBoundsTestEnable = VK_FALSE;
    state.minDepthBounds        = 0.0f;
    state.maxDepthBounds        = 1.0f;

    state.stencilTestEnable =
        (info.b_enable_front_face_stencil || info.b_enable_back_face_stencil) ? VK_TRUE : VK_FALSE;

    if (info.b_enable_front_face_stencil) {
        state.front.failOp = VulkanEnumTranslator::METoVKStencilOp(info.front_face_stencil_fail_stencil_op);
        state.front.passOp = VulkanEnumTranslator::METoVKStencilOp(info.front_face_pass_stencil_op);
        state.front.depthFailOp =
            VulkanEnumTranslator::METoVKStencilOp(info.front_face_depth_fail_stencil_op);
        state.front.compareOp   = VulkanEnumTranslator::METoVKCompareOp(info.front_face_stencil_test);
        state.front.compareMask = info.stencil_readmask;
        state.front.writeMask   = info.stencil_writemask;
        state.front.reference   = 0;
    } else {
        state.front.failOp      = VK_STENCIL_OP_KEEP;
        state.front.passOp      = VK_STENCIL_OP_KEEP;
        state.front.depthFailOp = VK_STENCIL_OP_KEEP;
        state.front.compareOp   = VK_COMPARE_OP_ALWAYS;
        state.front.compareMask = 0;
        state.front.writeMask   = 0;
        state.front.reference   = 0;
    }

    if (info.b_enable_back_face_stencil) {
        state.back.failOp = VulkanEnumTranslator::METoVKStencilOp(info.back_face_stencil_fail_stencil_op);
        state.back.passOp = VulkanEnumTranslator::METoVKStencilOp(info.back_face_pass_stencil_op);
        state.back.depthFailOp = VulkanEnumTranslator::METoVKStencilOp(info.back_face_depth_fail_stencil_op);
        state.back.compareOp   = VulkanEnumTranslator::METoVKCompareOp(info.back_face_stencil_test);
        state.back.compareMask = info.stencil_readmask;
        state.back.writeMask   = info.stencil_writemask;
        state.back.reference   = 0;
    } else {
        state.back.failOp      = VK_STENCIL_OP_KEEP;
        state.back.passOp      = VK_STENCIL_OP_KEEP;
        state.back.depthFailOp = VK_STENCIL_OP_KEEP;
        state.back.compareOp   = VK_COMPARE_OP_ALWAYS;
        state.back.compareMask = 0;
        state.back.writeMask   = 0;
        state.back.reference   = 0;
    }
    return state;
}

// Temporary native graphics-pipeline descriptions and their backing arrays, used only during creation.
struct VulkanGraphicsPipelineCreateData {
    Array<VkFormat>                            color_attachment_formats;
    Array<VkPipelineColorBlendAttachmentState> color_blend_attachments;
    Array<VkVertexInputBindingDescription>     vertex_bindings;
    Array<VkVertexInputAttributeDescription>   vertex_attributes;
    StaticArray<VkDynamicState, 2> dynamic_states = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};

    VkGraphicsPipelineCreateInfo vk_create_info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    VkPipelineRenderingCreateInfo        rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    VkPipelineVertexInputStateCreateInfo vertex_input{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO
    };
    VkPipelineInputAssemblyStateCreateInfo input_assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO
    };
    VkPipelineViewportStateCreateInfo      viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    VkPipelineRasterizationStateCreateInfo rasterization{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO
    };
    VkPipelineMultisampleStateCreateInfo  multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO
    };
    VkPipelineTessellationStateCreateInfo tessellation{
        VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO
    };
    VkPipelineDepthStencilStateCreateInfo depth_stencil{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO
    };
    VkPipelineColorBlendStateCreateInfo color_blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    VkPipelineDynamicStateCreateInfo    dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    bool                                uses_stencil_attachment = false;

    VulkanGraphicsPipelineCreateData() = default;
    // Internal pointers refer to this object's storage, so its address must remain stable.
    VulkanGraphicsPipelineCreateData(const VulkanGraphicsPipelineCreateData&)            = delete;
    VulkanGraphicsPipelineCreateData& operator=(const VulkanGraphicsPipelineCreateData&) = delete;
    VulkanGraphicsPipelineCreateData(VulkanGraphicsPipelineCreateData&&)                 = delete;
    VulkanGraphicsPipelineCreateData& operator=(VulkanGraphicsPipelineCreateData&&)      = delete;
};

void InitializeGraphicsAttachmentState(
    const GfxPsoCreateInfo&           create_info,
    VulkanGraphicsPipelineCreateData& create_data
) {
    const uint32 attachment_count = create_info.color_attachment_count;
    create_data.color_attachment_formats.resize(attachment_count);
    create_data.color_blend_attachments.resize(attachment_count);
    for (uint32 index = 0; index < attachment_count; ++index) {
        const RHIColorAttachmentInfo& attachment = create_info.color_attachments_info[index];
        create_data.color_attachment_formats[index] = VulkanEnumTranslator::METoVKFormat(attachment.pixel_format);
        create_data.color_blend_attachments[index]  = ToVulkanBlendAttachment(attachment.blend_state_info);
    }

    create_data.rendering.viewMask                = create_info.view_mask;
    create_data.rendering.colorAttachmentCount    = attachment_count;
    create_data.rendering.pColorAttachmentFormats = create_data.color_attachment_formats.data();
    create_data.rendering.depthAttachmentFormat =
        VulkanEnumTranslator::METoVKFormat(create_info.depth_stencil_format);

    // Only declare a stencil attachment when the PSO actually enables stencil tests.
    const bool depth_has_stencil = create_info.depth_stencil_format == PF_D32_SFLOAT_S8_UINT ||
                                   create_info.depth_stencil_format == PF_D24_UNORM_S8_UINT ||
                                   create_info.depth_stencil_format == PF_D16_UNORM_S8_UINT ||
                                   create_info.depth_stencil_format == PF_S8_UINT;
    const bool stencil_enabled = create_info.depth_stencil_info.b_enable_front_face_stencil ||
                                 create_info.depth_stencil_info.b_enable_back_face_stencil;
    create_data.uses_stencil_attachment = depth_has_stencil && stencil_enabled;
    create_data.rendering.stencilAttachmentFormat =
        create_data.uses_stencil_attachment ? create_data.rendering.depthAttachmentFormat : VK_FORMAT_UNDEFINED;

    create_data.color_blend.logicOp         = VK_LOGIC_OP_COPY;
    create_data.color_blend.logicOpEnable   = VK_FALSE;
    create_data.color_blend.attachmentCount = attachment_count;
    create_data.color_blend.pAttachments    = create_data.color_blend_attachments.data();
}

void InitializeGraphicsVertexInputState(
    const GfxPsoCreateInfo&           create_info,
    VulkanGraphicsPipelineCreateData& create_data
) {
    uint32 binding_index = 0;
    create_data.vertex_bindings.reserve(create_info.vertex_stream.bindings.size());
    uint32 attribute_count = 0;
    for (const VertexBinding& binding : create_info.vertex_stream.bindings) {
        attribute_count += binding.vertex_elements.size();
    }
    uint32 attribute_location = 0;
    create_data.vertex_attributes.reserve(attribute_count);
    for (const VertexBinding& binding : create_info.vertex_stream.bindings) {
        uint32 binding_stride   = 0;
        uint32 attribute_offset = 0;
        for (const VertexElement& attribute : binding.vertex_elements) {
            const VkFormat format = ToVulkanVertexFormat(attribute.format);
            if (format == VK_FORMAT_UNDEFINED) {
                throw std::invalid_argument("Invalid Vulkan vertex attribute format");
            }
            create_data.vertex_attributes.emplace_back(
                attribute_location++, binding_index, format, attribute_offset
            );
            attribute_offset += GetVertexFormatByteSize(attribute.format);
            binding_stride = attribute_offset;
        }
        create_data.vertex_bindings.emplace_back(
            binding_index, binding_stride, VulkanEnumTranslator::METoVKVertexInputRate(binding.input_rate)
        );
        ++binding_index;
    }
    create_data.vertex_input.vertexBindingDescriptionCount   = uint32(create_data.vertex_bindings.size());
    create_data.vertex_input.pVertexBindingDescriptions      = create_data.vertex_bindings.data();
    create_data.vertex_input.vertexAttributeDescriptionCount = uint32(create_data.vertex_attributes.size());
    create_data.vertex_input.pVertexAttributeDescriptions    = create_data.vertex_attributes.data();
}

void InitializeGraphicsPipelineCreateData(
    const GfxPsoCreateInfo&                          create_info,
    std::span<const VkPipelineShaderStageCreateInfo> shader_stages,
    VkPipelineLayout                                 layout,
    VulkanGraphicsPipelineCreateData&                create_data
) {
    InitializeGraphicsAttachmentState(create_info, create_data);
    InitializeGraphicsVertexInputState(create_info, create_data);

    create_data.input_assembly.topology =
        VulkanEnumTranslator::METoVKPrimitiveTopology(create_info.primitive_topology);
    create_data.input_assembly.primitiveRestartEnable = VK_FALSE;
    create_data.viewport.viewportCount                = create_info.multi_view_count;
    create_data.viewport.scissorCount                 = create_info.multi_view_count;
    create_data.rasterization                         = ToVulkanRasterizationState(create_info.rasterizer_info);
    create_data.multisample                           = ToVulkanMultisampleState(create_info.multisample_info);
    create_data.tessellation.patchControlPoints       = create_info.patch_control_points;
    create_data.depth_stencil                         = ToVulkanDepthStencilState(create_info.depth_stencil_info);
    create_data.dynamic.dynamicStateCount             = create_data.dynamic_states.size();
    create_data.dynamic.pDynamicStates                = create_data.dynamic_states.data();

    const bool uses_tessellation = std::ranges::any_of(
        shader_stages, [](const VkPipelineShaderStageCreateInfo& stage) {
            return stage.stage == VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
        }
    );
    VkGraphicsPipelineCreateInfo& info = create_data.vk_create_info;
    info.pNext               = &create_data.rendering;
    info.flags                         = 0;
    info.stageCount                    = static_cast<uint32>(shader_stages.size());
    info.pStages             = shader_stages.data();
    info.pVertexInputState   = &create_data.vertex_input;
    info.pInputAssemblyState = &create_data.input_assembly;
    info.pTessellationState  = uses_tessellation ? &create_data.tessellation : nullptr;
    info.pViewportState      = &create_data.viewport;
    info.pRasterizationState = &create_data.rasterization;
    info.pMultisampleState   = &create_data.multisample;
    info.pDepthStencilState  = &create_data.depth_stencil;
    info.pColorBlendState    = &create_data.color_blend;
    info.pDynamicState       = &create_data.dynamic;
    info.layout              = layout;
    info.basePipelineIndex   = -1;
}

} // namespace

PipelineHandle
VulkanDevice::CreatePipeline(GfxPsoCreateInfo&& _create_info, PipelineShaderInfo&& _shader_info) {
    if (!ValidateGraphicsPipelineCreateInfo(*this, m_device_info, _create_info, _shader_info)) {
        return {};
    }

    const Array<const SingleShaderInfo*> ordered_shaders =
        GetGraphicsShadersInStageOrder(_shader_info.shaders);
    VulkanPipelineBindingInfo binding_info =
        BuildVulkanPipelineBindingInfo(
            ordered_shaders, _shader_info, m_device_info.core_properties.core_1_0.limits.maxPushConstantsSize
        );

    UniquePtr<VulkanPipelineState> vk_pso = MakeUnique<VulkanPipelineState>(
        this, VulkanPipelineState::GFX, binding_info.active_argument_bits
    );
    Array<VkPipelineShaderStageCreateInfo> shader_stages = CreateVulkanShaderStages(m_device, ordered_shaders);
    auto shader_module_cleanup = OnScopeExit([&]() {
        for (const VkPipelineShaderStageCreateInfo& shader_stage : shader_stages) {
            vkDestroyShaderModule(m_device, shader_stage.module, nullptr);
        }
    });
    vk_pso->m_argument_infos = std::move(binding_info.argument_infos);
    InitializeVulkanPipelineLayout(*vk_pso, binding_info);

    VulkanGraphicsPipelineCreateData create_data;
    InitializeGraphicsPipelineCreateData(
        _create_info, shader_stages, vk_pso->GetPipelineLayout(), create_data
    );
    VkPipelineCreateFlags2CreateInfoKHR flags2{VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR};
    if (vk_pso->GetDescriptorMode() == VulkanDescriptorMode::Heap) {
        for (auto& stage : shader_stages)
            stage.pNext = &vk_pso->binding_template->heap_mapping_info;
        flags2.flags                     = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT;
        flags2.pNext                     = create_data.vk_create_info.pNext;
        create_data.vk_create_info.pNext = &flags2;
    } else if (vk_pso->GetDescriptorMode() == VulkanDescriptorMode::Buffer) {
        create_data.vk_create_info.flags |= VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
    }
    vk_pso->b_uses_stencil_attachment = create_data.uses_stencil_attachment;
    vk_pso->view_mask                 = _create_info.view_mask;
    VK_CHECK_RESULT(vkCreateGraphicsPipelines(
        m_device, VK_NULL_HANDLE, 1, &create_data.vk_create_info, nullptr, &vk_pso->m_pipeline
    ));

    return PipelineHandle{vk_pso.release()};
}

PipelineHandle VulkanDevice::CreatePipeline(PipelineShaderInfo&& shader_info) {
    if (const std::string_view error = ValidateComputeShaderStages(shader_info.shaders); !error.empty()) {
        LOG_ERROR("Cannot create Vulkan compute pipeline: {}", error);
        return {};
    }
    const SingleShaderInfo* shader = &shader_info.shaders.front();
    const std::span<const SingleShaderInfo* const> stages(&shader, 1);
    VulkanPipelineBindingInfo binding_info =
        BuildVulkanPipelineBindingInfo(
            stages, shader_info, m_device_info.core_properties.core_1_0.limits.maxPushConstantsSize
        );
    UniquePtr<VulkanPipelineState> vk_pso =
        MakeUnique<VulkanPipelineState>(this, VulkanPipelineState::Compute, binding_info.active_argument_bits);
    Array<VkPipelineShaderStageCreateInfo> shader_stages = CreateVulkanShaderStages(m_device, stages);
    auto shader_module_cleanup = OnScopeExit([&]() {
        vkDestroyShaderModule(m_device, shader_stages.front().module, nullptr);
    });
    vk_pso->m_argument_infos = std::move(binding_info.argument_infos);
    InitializeVulkanPipelineLayout(*vk_pso, binding_info);

    VkComputePipelineCreateInfo pipeline_create_info{};
    pipeline_create_info.sType             = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline_create_info.flags             = 0;
    pipeline_create_info.stage             = shader_stages.front();
    pipeline_create_info.layout            = vk_pso->GetPipelineLayout();
    pipeline_create_info.basePipelineIndex = -1;
    VkPipelineCreateFlags2CreateInfoKHR flags2{VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR};
    if (vk_pso->GetDescriptorMode() == VulkanDescriptorMode::Heap) {
        pipeline_create_info.stage.pNext = &vk_pso->binding_template->heap_mapping_info;
        flags2.flags                     = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT;
        pipeline_create_info.pNext       = &flags2;
    } else if (vk_pso->GetDescriptorMode() == VulkanDescriptorMode::Buffer) {
        pipeline_create_info.flags |= VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
    }
    VK_CHECK_RESULT(vkCreateComputePipelines(
        m_device, VK_NULL_HANDLE, 1, &pipeline_create_info, nullptr, &vk_pso->m_pipeline
    ));

    return PipelineHandle{vk_pso.release()};
}

} // namespace Moer::Render
