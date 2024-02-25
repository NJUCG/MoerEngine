#include "config.h"

#include "config/ConfigManager.h"
#include "log/LogSystem.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "rhi/RHIResourceInitilizer.h"
#include "rhi/vulkan/misc/VulkanMacroUtils.h"
#include "misc/MacroUtils.h"
#include "misc/STL.h"

#include "rhi/vulkan/VulkanRHI.h"
#include "VulkanCommand.h"

#include "VulkanRHIResource.h"
#include "VulkanRHIInitializer.h"
#include "VulkanExtension.h"

#include "shader/Shader.h"

#include "VulkanDebug.h"
#include "VulkanUtil.h"

#include "VulkanDevice.h"
#include "VulkanSwapChain.h"
#include "VulkanDescriptor.h"
#include "VulkanPipelineResourceCache.h"

#include "shader/Shader.h"
#include "shader/ShaderResource.h"
#include "vulkan/vulkan_core.h"
#include "window/WindowContext.h"

#include <cstdint>
#include <string>
#include <type_traits>

namespace VkUtil = Moer::RHI::Vulkan::Util;

VulkanRHIImpl::VulkanRHIImpl()
    : m_instance(VK_NULL_HANDLE), m_surface(VK_NULL_HANDLE),
      m_device(nullptr), m_main_viewport(nullptr) {
    LOG_INFO("Built with Vulkan header version {0:d}.{1:d}.{2:d}", VK_API_VERSION_MAJOR(VK_HEADER_VERSION_COMPLETE), VK_API_VERSION_MINOR(VK_HEADER_VERSION_COMPLETE), VK_API_VERSION_PATCH(VK_HEADER_VERSION_COMPLETE));
    rhi_type = ERHIType::Vulkan;
}

void VulkanRHIImpl::Initialize(const RHIInitInfo& _init) {
    //todo: need more elegant way
    max_frame_in_flight = _init.max_frame_in_flight;

    CreateInstance();
    InitSurface(Moer::WindowContext::GetMainWindow());
    InitVulkan();
}

void VulkanRHIImpl::PostInit() {
    LOG_INFO("VulkanRHIImpl::PostInit()");
}

void VulkanRHIImpl::ShutDown() {
    vkDeviceWaitIdle(m_device->GetDevice());
    CHECK_AND_DELETE(m_main_viewport);
    CHECK_AND_DELETE(m_device);
}

#pragma region resources creation
RHISamplerRef  VulkanRHIImpl::RHICreateSampler(const RHISamplerCreateInfo& _initializer) {
    VulkanRHISampler* vk_sampler = new VulkanRHISampler();
    vk_sampler->GenerateSamplerFromInitializer(m_device, _initializer);

    return RHISamplerRef(vk_sampler);
}

RHIRasterizationStateRef VulkanRHIImpl::RHICreateRasterizationState(const RHIRasterizeInfo& _init) {
    VulkanRHIRasterizationState* vk_rasterization_state = new VulkanRHIRasterizationState();
    vk_rasterization_state->GenerateRasterizationStateFromInitializer(_init);

    return RHIRasterizationStateRef(vk_rasterization_state);
}

RHIDepthStencilStateRef VulkanRHIImpl::RHICreateDepthStencilState(const RHIDepthStencilStateInfo& _init) {
    VulkanRHIDepthStencilState* vk_depth_stencil_state = new VulkanRHIDepthStencilState();
    vk_depth_stencil_state->GenerateDepthStencilStateFromInitializer(_init);

    return RHIDepthStencilStateRef(vk_depth_stencil_state);
}

RHIMultisampleStateRef VulkanRHIImpl::RHICreateMultiSampleState(const RHIMultisampleStateInfo& _init) {
    VulkanRHIMultisampleState* vk_multisample_state = new VulkanRHIMultisampleState();
    vk_multisample_state->GenerateMultisampleStateFromInitializer(_init);

    return RHIMultisampleStateRef(vk_multisample_state);
}

RHIBlendStateRef VulkanRHIImpl::RHICreateBlendState(const RHIBlendStateInfo& _init) {
    VulkanRHIBlendState* vk_blend_state = new VulkanRHIBlendState();
    vk_blend_state->GenerateBlendStateFromInitializer(_init);

    return RHIBlendStateRef(vk_blend_state);
}

RHIVertexInputStateRef VulkanRHIImpl::RHICreateVertexInputState(const VertexInputStateInitializerList& _init) {
    VulkanRHIVertexInputState* vk_input_state = new VulkanRHIVertexInputState();
    vk_input_state->GenerateVertexInputStateFromInitializer(_init);

    return RHIVertexInputStateRef(vk_input_state);
}

// RHIVertexShaderRef VulkanRHIImpl::RHICreateVertexShader(const Shader* shader) {
//     auto* vk_shader            = new VulkanRHIVertexShader(shader);
//     vk_shader->m_shader_module = VkUtil::CreateShaderModule(shader->GetCodeEntry()->code, m_device->GetDevice());

//     return RHIVertexShaderRef(vk_shader);
// }

// RHIFragmentShaderRef VulkanRHIImpl::RHICreateFragmentShader(const Shader* shader) {
//     auto* vk_shader            = new VulkanRHIFragmentShader(shader);
//     vk_shader->m_shader_module = VkUtil::CreateShaderModule(shader->GetCodeEntry()->code, m_device->GetDevice());

//     return RHIFragmentShaderRef(vk_shader);
// }

// RHIGeometryShaderRef VulkanRHIImpl::RHICreateGeometryShader(const Shader* shader) {
//     auto* vk_shader            = new VulkanRHIGeometryShader(shader);
//     vk_shader->m_shader_module = VkUtil::CreateShaderModule(shader->GetCodeEntry()->code, m_device->GetDevice());

//     return RHIGeometryShaderRef(vk_shader);
// }

// RHIMeshShaderRef VulkanRHIImpl::RHICreateMeshShader(const Shader* shader) {
//     auto* vk_shader            = new VulkanRHIMeshShader(shader);
//     vk_shader->m_shader_module = VkUtil::CreateShaderModule(shader->GetCodeEntry()->code, m_device->GetDevice());

//     return RHIMeshShaderRef(vk_shader);
// }

// RHIAmplificationShaderRef VulkanRHIImpl::RHICreateAmplificationShader(const Shader* shader) {
//     auto* vk_shader            = new VulkanRHIAmplificationShader(shader);
//     vk_shader->m_shader_module = VkUtil::CreateShaderModule(shader->GetCodeEntry()->code, m_device->GetDevice());
//     return RHIAmplificationShaderRef(vk_shader);
// }

// RHIComputeShaderRef VulkanRHIImpl::RHICreateComputeShader(const Shader* shader) {
//     auto* vk_shader            = new VulkanRHIComputeShader(shader);
//     vk_shader->m_shader_module = VkUtil::CreateShaderModule(shader->GetCodeEntry()->code, m_device->GetDevice());
//     return RHIComputeShaderRef(vk_shader);
// }

RHIVertexShaderRef VulkanRHIImpl::RHICreateVertexShader(const class ShaderCodeEntry* code_entry, const Shader* shader) {
    auto* vk_shader            = new VulkanRHIVertexShader(shader);
    vk_shader->m_shader_module = VkUtil::CreateShaderModule(code_entry->code, m_device->GetDevice());

    return RHIVertexShaderRef(vk_shader);
}

RHIFragmentShaderRef VulkanRHIImpl::RHICreateFragmentShader(const class ShaderCodeEntry* code_entry, const Shader* shader) {
    auto* vk_shader            = new VulkanRHIFragmentShader(shader);
    vk_shader->m_shader_module = VkUtil::CreateShaderModule(code_entry->code, m_device->GetDevice());

    return RHIFragmentShaderRef(vk_shader);
}

RHIGeometryShaderRef VulkanRHIImpl::RHICreateGeometryShader(const class ShaderCodeEntry* code_entry, const Shader* shader) {
    auto* vk_shader            = new VulkanRHIGeometryShader(shader);
    vk_shader->m_shader_module = VkUtil::CreateShaderModule(code_entry->code, m_device->GetDevice());

    return RHIGeometryShaderRef(vk_shader);
}

RHIMeshShaderRef VulkanRHIImpl::RHICreateMeshShader(const class ShaderCodeEntry* code_entry, const Shader* shader) {
    auto* vk_shader            = new VulkanRHIMeshShader(shader);
    vk_shader->m_shader_module = VkUtil::CreateShaderModule(code_entry->code, m_device->GetDevice());

    return RHIMeshShaderRef(vk_shader);
}

RHIAmplificationShaderRef VulkanRHIImpl::RHICreateAmplificationShader(const class ShaderCodeEntry* code_entry, const Shader* shader) {
    auto* vk_shader            = new VulkanRHIAmplificationShader(shader);
    vk_shader->m_shader_module = VkUtil::CreateShaderModule(code_entry->code, m_device->GetDevice());

    return RHIAmplificationShaderRef(vk_shader);
}

RHIComputeShaderRef VulkanRHIImpl::RHICreateComputeShader(const class ShaderCodeEntry* code_entry, const Shader* shader) {
    auto* vk_shader            = new VulkanRHIComputeShader(shader);
    vk_shader->m_shader_module = VkUtil::CreateShaderModule(code_entry->code, m_device->GetDevice());

    return RHIComputeShaderRef(vk_shader);
}

RHIShaderLibraryRef VulkanRHIImpl::RHICreateShaderLibrary(EShaderPlatform _platform, const std::string& _file_path, const std::string& name) { return RHIShaderLibraryRef{}; }

RHIFenceRef VulkanRHIImpl::RHICreateFence(const RHIFenceCreateInfo& _info) {

    VulkanRHIFence* vk_fence = new VulkanRHIFence(m_device, _info.usage);

    return RHIFenceRef(vk_fence);
}

RHIShaderBoundStateRef VulkanRHIImpl::RHICreateShaderBoundStage(
    RHIVertexInputState* _vertex_input,
    RHIVertexShader*     _vertex_shader,
    RHIFragmentShader*   _fragment_shader,
    RHIGeometryShader*   _geometry_shader) {

    auto* input = new RHIShaderBoundStateInput(_vertex_input, _vertex_shader, _fragment_shader, _geometry_shader);

    return RHIShaderBoundStateRef(input);
}

RHIGraphicsPipelineStateRef VulkanRHIImpl::RHICreateGraphicsPipelineState(const RHIGraphicsPipelineStateInfo& _init) {
    VulkanRHIGraphicsPipelineState* vk_pso = new VulkanRHIGraphicsPipelineState();

    uint32_t attachment_count = _init.CalcValidColorAttachmentCount();

    // rendering create info
    VkPipelineRenderingCreateInfo rendering_create_info{};
    rendering_create_info.sType                = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering_create_info.pNext                = nullptr;
    rendering_create_info.viewMask             = 0;
    rendering_create_info.colorAttachmentCount = attachment_count;
    Moer::Array<VkFormat> color_attachment_formats(attachment_count);
    for (int i = 0; i < attachment_count; ++i) {
        color_attachment_formats[i] = VkFormat(_init.color_attachment_formats[i]);
    }
    rendering_create_info.pColorAttachmentFormats = color_attachment_formats.data();
    rendering_create_info.depthAttachmentFormat   = VulkanEnumTranslator::METoVKFormat(_init.depth_stencil_format);
    rendering_create_info.stencilAttachmentFormat = VulkanEnumTranslator::METoVKFormat(_init.depth_stencil_format);

    // color blend state
    VkPipelineColorBlendStateCreateInfo color_blend_state{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};

    auto* vk_blend_state = static_cast<VulkanRHIBlendState*>(_init.blend_state.Get());
    if (!vk_blend_state) LOG_CRITICAL("RHICreateGraphicsPipelineState: Blend state is nullptr!");
    color_blend_state.logicOp         = VK_LOGIC_OP_COPY;
    color_blend_state.logicOpEnable   = VK_FALSE;
    color_blend_state.attachmentCount = attachment_count;
    color_blend_state.pAttachments    = vk_blend_state->GetAttachments();
    // shader stage
    auto shader_stages = VulkanRHIGraphicsPipelineState::METoVKShaderStageCreateInfo(_init.shader_stage);

    // vertex input state
    auto vertex_input_state = VulkanRHIGraphicsPipelineState::METoVKVertexInputStateCreateInfo(_init.shader_stage.p_vertex_input_state);

    // input assembly
    VkPipelineInputAssemblyStateCreateInfo input_assembly_state{};
    input_assembly_state.sType                  = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly_state.pNext                  = nullptr;
    input_assembly_state.flags                  = 0;
    input_assembly_state.topology               = VulkanEnumTranslator::METoVKPrimitiveTopology(_init.primitive_topology);
    input_assembly_state.primitiveRestartEnable = VK_FALSE;

    // viewport state
    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.pNext         = nullptr;
    viewport_state.flags         = 0;
    viewport_state.viewportCount = _init.multi_view_count;
    viewport_state.scissorCount  = _init.multi_view_count;

#define CHECK_AND_SET(ptr, state, msg) \
    if (ptr)                           \
        state = ptr->GetHandle();      \
    else                               \
        LOG_CRITICAL(msg);

    // rasterization state
    VkPipelineRasterizationStateCreateInfo rasterization_state{};

    auto* vk_rasterizer_state = static_cast<VulkanRHIRasterizationState*>(_init.rasterizer_state.Get());
    CHECK_AND_SET(vk_rasterizer_state, rasterization_state, "RHICreateGraphicsPipelineState: rasterization state is nullptr!");

    // multisample state
    VkPipelineMultisampleStateCreateInfo multisample_state{};

    auto* vk_multisample_state = static_cast<VulkanRHIMultisampleState*>(_init.multisample_state.Get());
    CHECK_AND_SET(vk_multisample_state, multisample_state, "RHICreateGraphicsPipelineState: multisample state is nullptr!");

    // depth stencil state
    VkPipelineDepthStencilStateCreateInfo depth_stencil_state{};

    auto* vk_depth_stencil_state = static_cast<VulkanRHIDepthStencilState*>(_init.depth_stencil_state.Get());
    CHECK_AND_SET(vk_depth_stencil_state, depth_stencil_state, "RHICreateGraphicsPipelineState: depth stencil state is nullptr!");

#undef CHECK_AND_SET

    // dynamic state
    Moer::StaticArray<VkDynamicState, 2> states = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo     dynamic_state{};
    dynamic_state.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state.pNext             = nullptr;
    dynamic_state.flags             = 0;
    dynamic_state.dynamicStateCount = states.size();
    dynamic_state.pDynamicStates    = states.data();

    // pipeline layout
    auto shader_info_list = VulkanRHIGraphicsPipelineState::GetShaderInfoList(_init.shader_stage);// MARK...

    Moer::Array<TDescriptorSetLayoutInfo> layout_mappings;
    Moer::Array<VkPushConstantRange>      push_constant_ranges;

    // find max set index
    int8_t max_set = -1;
    for (const auto* meta_shader : shader_info_list) {
        auto layout_infos = meta_shader->GetRootParametersLayoutInfo().GetLayoutInfos();
        for (const auto& info : layout_infos) {
            max_set = std::max(max_set, info.space);
        }
    }
    layout_mappings.resize(max_set + 1, {});

    // construct layout mappings
    for (const auto* meta_shader : shader_info_list) {
        auto layout_infos   = meta_shader->GetRootParametersLayoutInfo().GetLayoutInfos();
        auto constant_infos = meta_shader->GetRootParametersLayoutInfo().GetConstantsInfos();

        for (const auto& info : layout_infos) {
            VkDescriptorSetLayoutBinding binding{};
            binding.binding         = info.slot;
            binding.descriptorType  = VulkanEnumTranslator::METoVKDescriptorType(info.type, info.resource_type);
            binding.descriptorCount = 1;
            binding.stageFlags |= VulkanEnumTranslator::METoVKShaderStageFlags(meta_shader->GetShaderType());
            binding.pImmutableSamplers = nullptr;

            layout_mappings[info.space].second.push_back(std::move(binding));
        }

        // constants
        for (const auto& info : constant_infos) {
            VkPushConstantRange range{};
            range.stageFlags |= VulkanEnumTranslator::METoVKShaderStageFlags(meta_shader->GetShaderType());
            range.offset = info.offset;
            range.size   = info.stride;
            push_constant_ranges.push_back(range);
        }
    }

    // generate descriptor set layouts
    vk_pso->CreateResourceCache();
    vk_pso->GenerateDescriptorSetLayouts(m_device, layout_mappings);

    auto layouts = vk_pso->m_descriptor_sets_layout->GetLayouts();
    // create pipeline layout
    VkPipelineLayoutCreateInfo pipeline_layout_create_info{};
    pipeline_layout_create_info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_create_info.pNext                  = nullptr;
    pipeline_layout_create_info.flags                  = 0;
    pipeline_layout_create_info.setLayoutCount         = layouts.size();
    pipeline_layout_create_info.pSetLayouts            = layouts.data();
    pipeline_layout_create_info.pushConstantRangeCount = push_constant_ranges.size();
    pipeline_layout_create_info.pPushConstantRanges    = push_constant_ranges.data();

    vkCreatePipelineLayout(m_device->GetDevice(), &pipeline_layout_create_info, nullptr, &vk_pso->m_pipeline_layout);

    VkGraphicsPipelineCreateInfo pipeline_create_info{};
    pipeline_create_info.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_create_info.pNext               = &rendering_create_info;
    pipeline_create_info.flags               = 0;
    pipeline_create_info.stageCount          = shader_stages.size();
    pipeline_create_info.pStages             = shader_stages.data();
    pipeline_create_info.pVertexInputState   = &vertex_input_state;
    pipeline_create_info.pInputAssemblyState = &input_assembly_state;
    pipeline_create_info.pTessellationState  = nullptr;
    pipeline_create_info.pViewportState      = &viewport_state;
    pipeline_create_info.pRasterizationState = &rasterization_state;
    pipeline_create_info.pMultisampleState   = &multisample_state;
    pipeline_create_info.pDepthStencilState  = &depth_stencil_state;
    pipeline_create_info.pColorBlendState    = &color_blend_state;
    pipeline_create_info.pDynamicState       = &dynamic_state;
    pipeline_create_info.layout              = vk_pso->m_pipeline_layout;
    pipeline_create_info.renderPass          = nullptr;
    pipeline_create_info.subpass             = 0;
    pipeline_create_info.basePipelineHandle  = nullptr;// MARK...
    pipeline_create_info.basePipelineIndex   = -1;

    VK_CHECK_RESULT(vkCreateGraphicsPipelines(m_device->GetDevice(), nullptr, 1, &pipeline_create_info, nullptr, &vk_pso->m_pipeline));

    return RHIGraphicsPipelineStateRef(vk_pso);
}
VkBlendOp METoVKBlendOp(EBlendOperation _blend_op) {
    switch (_blend_op) {
        case BO_ADD:
            return VK_BLEND_OP_ADD;
        case BO_SUBTRACT:
            return VK_BLEND_OP_SUBTRACT;
        case BO_REVERSE_SUBTRACT:
            return VK_BLEND_OP_REVERSE_SUBTRACT;
        case BO_MIN:
            return VK_BLEND_OP_MIN;
        case BO_MAX:
            return VK_BLEND_OP_MAX;
        default:
            LOG_CRITICAL("Unsupported color blend operation: {}", static_cast<uint32_t>(_blend_op));
            return VK_BLEND_OP_MAX_ENUM;
    }
}
VkBlendFactor METoVKBlendFactor(EBlendFactor _blend_factor) {
    switch (_blend_factor) {
        case BF_ZERO:
            return VK_BLEND_FACTOR_ZERO;
        case BF_ONE:
            return VK_BLEND_FACTOR_ONE;
        case BF_SRC_COLOR:
            return VK_BLEND_FACTOR_SRC_COLOR;
        case BF_ONE_MINUS_SRC_COLOR:
            return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case BF_DST_COLOR:
            return VK_BLEND_FACTOR_DST_COLOR;
        case BF_ONE_MINUS_DST_COLOR:
            return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case BF_SRC_ALPHA:
            return VK_BLEND_FACTOR_SRC_ALPHA;
        case BF_ONE_MINUS_SRC_ALPHA:
            return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case BF_DST_ALPHA:
            return VK_BLEND_FACTOR_DST_ALPHA;
        case BF_ONE_MINUS_DST_ALPHA:
            return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case BF_CONSTANT_ALPHA:
            return VK_BLEND_FACTOR_CONSTANT_ALPHA;
        case BF_ONE_MINUS_CONSTANT_ALPHA:
            return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        case BF_SRC1_COLOR:
            return VK_BLEND_FACTOR_SRC1_COLOR;
        case BF_ONE_MINUS_SRC1_COLOR:
            return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
        case BF_SRC1_ALPHA:
            return VK_BLEND_FACTOR_SRC1_ALPHA;
        case BF_ONE_MINUS_SRC1_ALPHA:
            return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
        default:
            LOG_CRITICAL("Unsupported color blend factor: {}", static_cast<uint32_t>(_blend_factor));
            return VK_BLEND_FACTOR_MAX_ENUM;
    }
}
RHIGraphicsPipelineStateRef VulkanRHIImpl::RHICreateGraphicsPSO(RHIGraphicsPSOCreateInfo&& _init) {
    VulkanRHIGraphicsPipelineState* vk_pso = new VulkanRHIGraphicsPipelineState();

    assert(_init.finalized && "RHICreateGraphicsPSO: PSO is not finalized!");
    uint32_t attachment_count = _init.color_attachment_count;

    Moer::Array<VkFormat> color_attachment_formats(attachment_count);

    for (int i = 0; i < attachment_count; ++i) {
        color_attachment_formats[i] = VkFormat(_init.color_attachments_info[i].pixel_format);
    }
    VkPipelineRenderingCreateInfo rendering_create_info{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering_create_info.pNext                   = nullptr;
    rendering_create_info.viewMask                = 0;
    rendering_create_info.colorAttachmentCount    = attachment_count;
    rendering_create_info.pColorAttachmentFormats = color_attachment_formats.data();

    auto to_vk_blend_attachment = [](const RHIBlendAttachmentInfo& _info) {
        VkPipelineColorBlendAttachmentState state{};
        state.blendEnable =
            (_info.color_blend_op != BO_ADD || _info.color_dst_blend_factor != BF_ZERO || _info.color_src_blend_factor != BF_ONE ||
             _info.alpha_blend_op != BO_ADD || _info.alpha_dst_blend_factor != BF_ZERO || _info.alpha_src_blend_factor != BF_ONE) ?
                VK_TRUE :
                VK_FALSE;
        state.srcColorBlendFactor = METoVKBlendFactor(_info.color_src_blend_factor);
        state.dstColorBlendFactor = METoVKBlendFactor(_info.color_dst_blend_factor);
        state.colorBlendOp        = METoVKBlendOp(_info.color_blend_op);
        state.srcAlphaBlendFactor = METoVKBlendFactor(_info.alpha_src_blend_factor);
        state.dstAlphaBlendFactor = METoVKBlendFactor(_info.alpha_dst_blend_factor);
        state.alphaBlendOp        = METoVKBlendOp(_info.alpha_blend_op);
        state.colorWriteMask      = (_info.color_write_mask & CW_RED) ? VK_COLOR_COMPONENT_R_BIT : 0;
        state.colorWriteMask |= (_info.color_write_mask & CW_GREEN) ? VK_COLOR_COMPONENT_G_BIT : 0;
        state.colorWriteMask |= (_info.color_write_mask & CW_BLUE) ? VK_COLOR_COMPONENT_B_BIT : 0;
        state.colorWriteMask |= (_info.color_write_mask & CW_ALPHA) ? VK_COLOR_COMPONENT_A_BIT : 0;
        return std::move(state);
    };
    Moer::Array<VkPipelineColorBlendAttachmentState> color_blend_attachments(attachment_count);
    for (int i = 0; i < attachment_count; ++i) {
        color_blend_attachments[i] = to_vk_blend_attachment(_init.color_attachments_info[i].blend_state_info);
    }
    // color blend state
    VkPipelineColorBlendStateCreateInfo color_blend_state{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};

    color_blend_state.logicOp         = VK_LOGIC_OP_COPY;
    color_blend_state.logicOpEnable   = VK_FALSE;
    color_blend_state.attachmentCount = attachment_count;
    color_blend_state.pAttachments    = color_blend_attachments.data();

    // shader stage
    auto& shader_info_group = _init.shader_infos.work_flow;

    Moer::Array<VkPipelineShaderStageCreateInfo> shader_stages;
    Moer::Array<const Shader*>                   shader_info_list;

#define FILL_SHADER_DATA(rhi_shader, target_shader_stage, target_info)                                   \
    shader_stage_info.stage  = target_shader_stage;                                                      \
    shader_stage_info.module = rhi_shader->GetHandle();                                                  \
    shader_stage_info.pName  = rhi_shader->GetMetaShader()->GetShaderMetaType()->GetEntryPoint().data(); \
    shader_stages.push_back(shader_stage_info);                                                          \
    shader_info_list.push_back(rhi_shader->GetMetaShader());

    VkPipelineVertexInputStateCreateInfo vertex_input_state{};

    if (_init.shader_infos.IsVertexWorkFlow()) {

        auto work_flow = std::get<RHIGraphicsShaderInputInfo::t_vertex_work_flow>(shader_info_group);
        assert(work_flow.vertex_shader && work_flow.fragment_shader && "RHICreateGraphicsPSO: vertex shader is nullptr!");

        VkPipelineShaderStageCreateInfo shader_stage_info{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        shader_stage_info.pSpecializationInfo = VK_NULL_HANDLE;
        shader_stage_info.pNext               = VK_NULL_HANDLE;
        shader_stage_info.flags               = 0;

        {
            auto* vk_vert_shader = static_cast<VulkanRHIVertexShader*>(work_flow.vertex_shader);
            FILL_SHADER_DATA(vk_vert_shader, VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_VERTEX_BIT);
        }

        {
            auto* vk_frag_shader = static_cast<VulkanRHIFragmentShader*>(work_flow.fragment_shader);
            FILL_SHADER_DATA(vk_frag_shader, VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_FRAGMENT_BIT);
        }

        if (work_flow.geometry_shader) {
            auto* vk_geom_shader = static_cast<VulkanRHIGeometryShader*>(work_flow.geometry_shader);
            FILL_SHADER_DATA(vk_geom_shader, VK_SHADER_STAGE_GEOMETRY_BIT, VK_SHADER_STAGE_GEOMETRY_BIT);
        }
        vertex_input_state = std::move(VulkanRHIGraphicsPipelineState::METoVKVertexInputStateCreateInfo(work_flow.vertex_input_state));

    } else {
        auto work_flow = std::get<RHIGraphicsShaderInputInfo::t_mesh_work_flow>(shader_info_group);
        assert(work_flow.mesh_shader && work_flow.fragment_shader && "RHICreateGraphicsPSO: mesh shader is nullptr!");

        VkPipelineShaderStageCreateInfo shader_stage_info{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        shader_stage_info.pSpecializationInfo = VK_NULL_HANDLE;
        shader_stage_info.pNext               = VK_NULL_HANDLE;
        shader_stage_info.flags               = 0;

        {
            auto* vk_mesh_shader = static_cast<VulkanRHIMeshShader*>(work_flow.mesh_shader);
            FILL_SHADER_DATA(vk_mesh_shader, VK_SHADER_STAGE_MESH_BIT_NV, VK_SHADER_STAGE_MESH_BIT_NV);
        }

        {
            auto* vk_frag_shader = static_cast<VulkanRHIFragmentShader*>(work_flow.fragment_shader);
            FILL_SHADER_DATA(vk_frag_shader, VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_FRAGMENT_BIT);
        }

        if (work_flow.amplification_shader) {
            auto* vk_amp_shader = static_cast<VulkanRHIAmplificationShader*>(work_flow.amplification_shader);
            FILL_SHADER_DATA(vk_amp_shader, VK_SHADER_STAGE_TASK_BIT_NV, VK_SHADER_STAGE_TASK_BIT_NV);
        }
    }
#undef FILL_SHADER_DATA
    // vertex input state

    // input assembly
    VkPipelineInputAssemblyStateCreateInfo input_assembly_state{};
    input_assembly_state.sType                  = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly_state.pNext                  = nullptr;
    input_assembly_state.flags                  = 0;
    input_assembly_state.topology               = VulkanEnumTranslator::METoVKPrimitiveTopology(_init.primitive_topology);
    input_assembly_state.primitiveRestartEnable = VK_FALSE;

    // viewport state
    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.pNext         = nullptr;
    viewport_state.flags         = 0;
    viewport_state.viewportCount = _init.multi_view_count;
    viewport_state.scissorCount  = _init.multi_view_count;
    // rasterization state
    VkPipelineRasterizationStateCreateInfo vk_rasterization_state{};

    auto to_rasterize_state = [](const RHIRasterizeInfo& info) {
        VkPipelineRasterizationStateCreateInfo state{};
        state.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        state.pNext                   = nullptr;
        state.flags                   = 0;
        state.depthClampEnable        = info.b_depth_clamp_enable ? VK_TRUE : VK_FALSE;
        state.rasterizerDiscardEnable = VK_FALSE;// MARK...
        state.polygonMode             = VulkanEnumTranslator::METoVKPolygonMode(info.fill_mode);
        state.cullMode                = VulkanEnumTranslator::METoVKCullModeFlags(info.cull_mode);
        state.frontFace               = VK_FRONT_FACE_COUNTER_CLOCKWISE;// MARK...
        state.depthBiasEnable         = info.b_depth_bias ? VK_TRUE : VK_FALSE;
        state.depthBiasConstantFactor = info.depth_bias;
        state.depthBiasClamp          = info.depth_bias_clamp;
        state.depthBiasSlopeFactor    = info.depth_bias_slop_factor;
        state.lineWidth               = 1.0f;
        return std::move(state);
    };

    vk_rasterization_state = to_rasterize_state(_init.rasterizer_info);
    // multisample state
    VkPipelineMultisampleStateCreateInfo multisample_state{};

    auto to_multi_sample_state = [](const RHIMultisampleStateInfo& info) {
        VkPipelineMultisampleStateCreateInfo state{};
        state.sType                 = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        state.pNext                 = nullptr;
        state.flags                 = 0;
        state.rasterizationSamples  = VulkanEnumTranslator::METoVKSampleCountFlagBits(info.sample_count);
        state.sampleShadingEnable   = VK_FALSE;
        state.minSampleShading      = 1.0f;
        state.pSampleMask           = nullptr;
        state.alphaToCoverageEnable = VK_FALSE;
        state.alphaToOneEnable      = VK_FALSE;
        return std::move(state);
    };
    auto vk_multisample_state = to_multi_sample_state(_init.multisample_info);

    // depth stencil state
    VkPipelineDepthStencilStateCreateInfo depth_stencil_state{};

    auto to_depth_stencil_state = [](const RHIDepthStencilStateInfo& info) {
        VkPipelineDepthStencilStateCreateInfo state{};
        state.sType                 = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        state.pNext                 = nullptr;
        state.flags                 = 0;
        state.depthTestEnable       = (info.b_enable_depth_write || info.depth_test_op == ECompareOption::CO_ALWAYS) ? VK_TRUE : VK_FALSE;
        state.depthWriteEnable      = info.b_enable_depth_write;
        state.depthCompareOp        = VulkanEnumTranslator::METoVKCompareOp(info.depth_test_op);
        state.depthBoundsTestEnable = VK_FALSE;// MARK...
        state.minDepthBounds        = 0.0f;
        state.maxDepthBounds        = 1.0f;

        state.stencilTestEnable = (info.b_enable_front_face_stencil || info.b_enable_back_face_stencil) ? VK_TRUE : VK_FALSE;
        state.front.failOp      = VulkanEnumTranslator::METoVKStencilOp(info.front_face_stencil_fail_stencil_op);
        state.front.passOp      = VulkanEnumTranslator::METoVKStencilOp(info.front_face_pass_stencil_op);
        state.front.depthFailOp = VulkanEnumTranslator::METoVKStencilOp(info.front_face_depth_fail_stencil_op);
        state.front.compareOp   = VulkanEnumTranslator::METoVKCompareOp(info.front_face_stencil_test);
        state.front.compareMask = info.stencil_readmask;
        state.front.writeMask   = info.stencil_writemask;
        state.front.reference   = 0;

        if (info.b_enable_back_face_stencil) {
            state.back.failOp      = VulkanEnumTranslator::METoVKStencilOp(info.back_face_stencil_fail_stencil_op);
            state.back.passOp      = VulkanEnumTranslator::METoVKStencilOp(info.back_face_pass_stencil_op);
            state.back.depthFailOp = VulkanEnumTranslator::METoVKStencilOp(info.back_face_depth_fail_stencil_op);
            state.back.compareOp   = VulkanEnumTranslator::METoVKCompareOp(info.back_face_stencil_test);
            state.back.compareMask = info.stencil_readmask;
            state.back.writeMask   = info.stencil_writemask;
            state.back.reference   = 0;
        } else {
            state.front = state.back;
        }
        return std::move(state);
    };
    auto vk_depth_stencil_state = to_depth_stencil_state(_init.depth_stencil_info);

    // dynamic state
    Moer::StaticArray<VkDynamicState, 2> states = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo     dynamic_state{};
    dynamic_state.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state.pNext             = nullptr;
    dynamic_state.flags             = 0;
    dynamic_state.dynamicStateCount = states.size();
    dynamic_state.pDynamicStates    = states.data();

    Moer::Array<TDescriptorSetLayoutInfo> layout_mappings;
    Moer::Array<VkPushConstantRange>      push_constant_ranges;

    // find max set index
    int8_t max_set = -1;
    for (const auto* meta_shader : shader_info_list) {
        auto layout_infos = meta_shader->GetRootParametersLayoutInfo().GetLayoutInfos();
        for (const auto& info : layout_infos) {
            max_set = std::max(max_set, info.space);
        }
    }
    layout_mappings.resize(max_set + 1, {});

    // construct layout mappings
    for (const auto* meta_shader : shader_info_list) {
        auto layout_infos   = meta_shader->GetRootParametersLayoutInfo().GetLayoutInfos();
        auto constant_infos = meta_shader->GetRootParametersLayoutInfo().GetConstantsInfos();

        for (const auto& info : layout_infos) {
            VkDescriptorSetLayoutBinding binding{};
            binding.binding         = info.slot;
            binding.descriptorType  = VulkanEnumTranslator::METoVKDescriptorType(info.type, info.resource_type);
            binding.descriptorCount = 1;
            binding.stageFlags |= VulkanEnumTranslator::METoVKShaderStageFlags(meta_shader->GetShaderType());
            binding.pImmutableSamplers = nullptr;

            layout_mappings[info.space].second.push_back(std::move(binding));
        }

        // constants
        for (const auto& info : constant_infos) {
            VkPushConstantRange range{};
            range.stageFlags |= VulkanEnumTranslator::METoVKShaderStageFlags(meta_shader->GetShaderType());
            range.offset = info.offset;
            range.size   = info.stride;
            push_constant_ranges.push_back(range);
        }
    }

    // generate descriptor set layouts
    vk_pso->CreateResourceCache();
    vk_pso->GenerateDescriptorSetLayouts(m_device, layout_mappings);

    auto layouts = vk_pso->m_descriptor_sets_layout->GetLayouts();
    // create pipeline layout
    VkPipelineLayoutCreateInfo pipeline_layout_create_info{};
    pipeline_layout_create_info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_create_info.pNext                  = nullptr;
    pipeline_layout_create_info.flags                  = 0;
    pipeline_layout_create_info.setLayoutCount         = layouts.size();
    pipeline_layout_create_info.pSetLayouts            = layouts.data();
    pipeline_layout_create_info.pushConstantRangeCount = push_constant_ranges.size();
    pipeline_layout_create_info.pPushConstantRanges    = push_constant_ranges.data();

    vkCreatePipelineLayout(m_device->GetDevice(), &pipeline_layout_create_info, nullptr, &vk_pso->m_pipeline_layout);

    VkGraphicsPipelineCreateInfo pipeline_create_info{};
    pipeline_create_info.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_create_info.pNext               = &rendering_create_info;
    pipeline_create_info.flags               = 0;
    pipeline_create_info.stageCount          = shader_stages.size();
    pipeline_create_info.pStages             = shader_stages.data();
    pipeline_create_info.pVertexInputState   = &vertex_input_state;
    pipeline_create_info.pInputAssemblyState = &input_assembly_state;
    pipeline_create_info.pTessellationState  = nullptr;
    pipeline_create_info.pViewportState      = &viewport_state;
    pipeline_create_info.pRasterizationState = &vk_rasterization_state;
    pipeline_create_info.pMultisampleState   = &multisample_state;
    pipeline_create_info.pDepthStencilState  = &depth_stencil_state;
    pipeline_create_info.pColorBlendState    = &color_blend_state;
    pipeline_create_info.pDynamicState       = &dynamic_state;
    pipeline_create_info.layout              = vk_pso->m_pipeline_layout;
    pipeline_create_info.renderPass          = nullptr;
    pipeline_create_info.subpass             = 0;
    pipeline_create_info.basePipelineHandle  = nullptr;// MARK...
    pipeline_create_info.basePipelineIndex   = -1;

    VK_CHECK_RESULT(vkCreateGraphicsPipelines(m_device->GetDevice(), nullptr, 1, &pipeline_create_info, nullptr, &vk_pso->m_pipeline));

    return RHIGraphicsPipelineStateRef(vk_pso);
}

RHIComputePipelineStateRef VulkanRHIImpl::RHICreateComputePipelineState(RHIComputeShader* _compute_shader) {
    VulkanRHIComputePipelineState* vk_pso = new VulkanRHIComputePipelineState();

    auto* vk_shader = static_cast<VulkanRHIComputeShader*>(_compute_shader);
    if (!vk_shader) LOG_CRITICAL("RHICreateComputePipelineState: Compute shader is nullptr!");

    VkPipelineShaderStageCreateInfo shader_stage{};
    shader_stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shader_stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    shader_stage.module = vk_shader->GetHandle();
    shader_stage.pName  = vk_shader->GetMetaShader()->GetShaderMetaType()->GetEntryPoint().data();

    Moer::Array<TDescriptorSetLayoutInfo> layout_mappings;
    Moer::Array<VkPushConstantRange>      push_constant_ranges;

    auto* meta_shader = vk_shader->GetMetaShader();
    // find max set index
    int8_t max_set      = -1;
    auto   layout_infos = meta_shader->GetRootParametersLayoutInfo().GetLayoutInfos();
    for (const auto& info : layout_infos) {
        max_set = std::max(max_set, info.space);
    }
    layout_mappings.resize(max_set + 1, {});

    auto constant_infos = meta_shader->GetRootParametersLayoutInfo().GetConstantsInfos();

    for (const auto& info : layout_infos) {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding         = info.slot;
        binding.descriptorType  = VulkanEnumTranslator::METoVKDescriptorType(info.type, info.resource_type);
        binding.descriptorCount = 1;
        binding.stageFlags |= VulkanEnumTranslator::METoVKShaderStageFlags(meta_shader->GetShaderType());
        binding.pImmutableSamplers = nullptr;

        layout_mappings[info.space].second.push_back(std::move(binding));
    }

    // constants
    for (const auto& info : constant_infos) {
        VkPushConstantRange range{};
        range.stageFlags |= VulkanEnumTranslator::METoVKShaderStageFlags(meta_shader->GetShaderType());
        range.offset = info.offset;
        range.size   = info.stride;

        push_constant_ranges.push_back(range);
    }

    vk_pso->CreateResourceCache();
    vk_pso->GenerateDescriptorSetLayouts(m_device, layout_mappings);

    auto layouts = vk_pso->m_descriptor_sets_layout->GetLayouts();

    VkPipelineLayoutCreateInfo pipeline_layout_create_info{};
    pipeline_layout_create_info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_create_info.pNext                  = nullptr;
    pipeline_layout_create_info.flags                  = 0;
    pipeline_layout_create_info.setLayoutCount         = layouts.size();
    pipeline_layout_create_info.pSetLayouts            = layouts.data();
    pipeline_layout_create_info.pushConstantRangeCount = push_constant_ranges.size();
    pipeline_layout_create_info.pPushConstantRanges    = push_constant_ranges.data();

    vkCreatePipelineLayout(m_device->GetDevice(), &pipeline_layout_create_info, nullptr, &vk_pso->m_pipeline_layout);

    VkComputePipelineCreateInfo pipeline_create_info{};
    pipeline_create_info.sType              = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline_create_info.pNext              = nullptr;
    pipeline_create_info.flags              = 0;
    pipeline_create_info.stage              = shader_stage;
    pipeline_create_info.layout             = vk_pso->m_pipeline_layout;
    pipeline_create_info.basePipelineHandle = nullptr;
    pipeline_create_info.basePipelineIndex  = -1;

    VK_CHECK_RESULT(vkCreateComputePipelines(m_device->GetDevice(), nullptr, 1, &pipeline_create_info, nullptr, &vk_pso->m_pipeline));

    return RHIComputePipelineStateRef(vk_pso);
}

RHIBufferRef VulkanRHIImpl::RHICreateBuffer(const RHIBufferCreateInfo& info) {
    RHIBufferInfo buffer_info{};
    buffer_info.size   = info.size;
    buffer_info.stride = info.stride;
    buffer_info.usage  = info.usage;

    VulkanRHIBuffer* vk_buffer = new VulkanRHIBuffer(buffer_info);

    VkBufferCreateInfo buffer_create_info{};
    buffer_create_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_create_info.pNext = nullptr;
    buffer_create_info.flags = 0;
    buffer_create_info.size  = info.size;
    // add device address flag for addressing buffer with 64-bit address
    buffer_create_info.usage                 = VulkanRHIBuffer::METoVKBufferUsageFlags(m_device, info.usage) | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    buffer_create_info.sharingMode           = VK_SHARING_MODE_EXCLUSIVE;
    buffer_create_info.queueFamilyIndexCount = 0;
    buffer_create_info.pQueueFamilyIndices   = nullptr;

    VmaAllocationCreateInfo alloc_create_info{};
    alloc_create_info.flags = VulkanMemoryManager::MEGenerateVmaMemoryFlags(info.usage);
    alloc_create_info.usage = VulkanMemoryManager::MEGenerateVmaMemoryUsage();

    VmaAllocator allocator = m_device->GetVmaAllocator();
    VK_CHECK_RESULT(vmaCreateBuffer(allocator, &buffer_create_info, &alloc_create_info, &vk_buffer->m_alloc.buffer, &vk_buffer->m_alloc.alloc, nullptr));

    return RHIBufferRef(vk_buffer);
}

void* VulkanRHIImpl::RHIMapBuffer(RHIBuffer* _buffer, uint64_t _offset, uint64_t _size) {
    auto* vk_buffer = static_cast<VulkanRHIBuffer*>(_buffer);
    VK_CHECK_NULLPTR(vk_buffer, "RHIMapBuffer: buffer to be mapped is nullptr!", return nullptr);

    VmaAllocator allocator = m_device->GetVmaAllocator();

    void* p_data;
    VK_CHECK_RESULT(vmaMapMemory(allocator, vk_buffer->m_alloc.alloc, &p_data));

    return p_data;
}
void VulkanRHIImpl::RHIUnmapBuffer(RHIBuffer* _buffer) {
    auto* vk_buffer = static_cast<VulkanRHIBuffer*>(_buffer);
    VK_CHECK_NULLPTR(vk_buffer, "RHIUnmapBuffer: buffer to be unmapped is nullptr!", return);

    VmaAllocator allocator = m_device->GetVmaAllocator();
    vmaUnmapMemory(allocator, vk_buffer->m_alloc.alloc);
    // vmaFlushAllocation(allocator, vk_buffer->m_alloc.alloc, 0, VK_WHOLE_SIZE);
}

RHITextureRef VulkanRHIImpl::RHICreateTexture(const RHITextureCreateInfo& info) {
    VulkanRHITexture* vk_texture = new VulkanRHITexture(info, m_device);

    VkImageCreateInfo image_create_info{};
    image_create_info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_create_info.pNext         = nullptr;
    image_create_info.flags         = 0;
    image_create_info.imageType     = VulkanRHITexture::METoVKImageType(info.dimension);
    image_create_info.format        = VulkanEnumTranslator::METoVKFormat(info.format);
    image_create_info.extent.width  = info.extent.x;
    image_create_info.extent.height = info.extent.y;
    image_create_info.extent.depth  = info.depth;
    image_create_info.mipLevels     = info.num_mips;
    image_create_info.arrayLayers   = info.array_size;
    image_create_info.samples       = VulkanEnumTranslator::METoVKSampleCountFlagBits(info.num_samples);
    image_create_info.tiling        = VK_IMAGE_TILING_OPTIMAL;// MARK...
    image_create_info.usage         = VulkanRHITexture::METoVKImageUsageFlags(info.usage);
    image_create_info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    // if (uint32_t(info.usage | ETextureUsageFlags::TRANSFER_DST) || uint32_t(info.usage | ETextureUsageFlags::TRANSFER_SRC)) {
    //     image_create_info.sharingMode           = VK_SHARING_MODE_CONCURRENT;
    //     image_create_info.queueFamilyIndexCount = 3;
    //     uint32_t queue_family_indices[]         = {m_device->GetQueueFamilyIndices().graphics.value(),
    //                                                m_device->GetQueueFamilyIndices().compute.value(),
    //                                                m_device->GetQueueFamilyIndices().transfer.value()};
    //     image_create_info.pQueueFamilyIndices   = queue_family_indices;
    // }

    image_create_info.initialLayout = VulkanEnumTranslator::METoVKImageLayout(info.layout);

    VmaAllocationCreateInfo alloc_create_info{};
    alloc_create_info.flags = 0;
    alloc_create_info.usage = VulkanMemoryManager::MEGenerateVmaMemoryUsage();

    VmaAllocator allocator = m_device->GetVmaAllocator();
    VK_CHECK_RESULT(vmaCreateImage(allocator, &image_create_info, &alloc_create_info, &vk_texture->m_alloc.image, &vk_texture->m_alloc.alloc, nullptr));
    // VmaAllocationInfo temp_info;
    // vmaGetAllocationInfo(allocator, vk_texture->GetAllocation(), &temp_info);
    return RHITextureRef(vk_texture);
};

RHISRVRef VulkanRHIImpl::RHICreateSRV(RHIViewableResource* _resource, const RHIViewInfo& _view_info) {

    auto create_texture_srv = [=]() {
        VulkanRHITextureSRV* vk_srv = new VulkanRHITextureSRV(m_device, _resource, _view_info);

        VkImageViewCreateInfo image_view_create_info{};
        image_view_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        image_view_create_info.pNext = nullptr;
        image_view_create_info.flags = 0;

        auto* vk_texture = static_cast<VulkanRHITexture*>(_resource);
        VK_CHECK_NULLPTR(vk_texture, "RHICreateSRV: resource to be viewed is nullptr!", return RHISRVRef{});

        image_view_create_info.image                           = vk_texture->GetHandle();
        image_view_create_info.viewType                        = VulkanEnumTranslator::METoVKImageViewType(_view_info.texture.srv.dimension);
        image_view_create_info.format                          = _view_info.texture.srv.format == PF_UNDEFINED ? VulkanEnumTranslator::METoVKFormat(vk_texture->GetUAVFormat()) : VulkanEnumTranslator::METoVKFormat(_view_info.texture.srv.format);
        image_view_create_info.components                      = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
        image_view_create_info.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;// MARK...
        image_view_create_info.subresourceRange.baseMipLevel   = _view_info.texture.srv.mip_min;
        image_view_create_info.subresourceRange.levelCount     = _view_info.texture.srv.mip_num;
        image_view_create_info.subresourceRange.baseArrayLayer = _view_info.texture.srv.array_min;
        image_view_create_info.subresourceRange.layerCount     = _view_info.texture.srv.array_num;

        VK_CHECK_RESULT(vkCreateImageView(m_device->GetDevice(), &image_view_create_info, nullptr, &vk_srv->m_view));

        return RHISRVRef(vk_srv);
    };

    auto create_buffer_srv = [=]() {
        auto*               vk_buffer = static_cast<VulkanRHIBuffer*>(_resource);
        VulkanRHIBufferSRV* vk_srv    = new VulkanRHIBufferSRV(m_device, _resource, _view_info);

        VkBufferViewCreateInfo buffer_view_create_info{};
        buffer_view_create_info.sType  = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
        buffer_view_create_info.pNext  = nullptr;
        buffer_view_create_info.flags  = 0;
        buffer_view_create_info.buffer = vk_buffer->GetHandle();
        buffer_view_create_info.format = VulkanEnumTranslator::METoVKFormat(_view_info.buffer.srv.format);
        buffer_view_create_info.offset = _view_info.buffer.srv.byte_offset;
        buffer_view_create_info.range  = _view_info.buffer.srv.stride * _view_info.buffer.srv.num_elements;

        VK_CHECK_RESULT(vkCreateBufferView(m_device->GetDevice(), &buffer_view_create_info, nullptr, &vk_srv->m_view));

        return RHISRVRef(vk_srv);
    };

    if (_view_info.IsBuffer()) {
        return create_buffer_srv();
    }
    return create_texture_srv();
}

RHIUAVRef VulkanRHIImpl::RHICreateUAV(RHIViewableResource* _resource, const RHIViewInfo& _view_info) {

    auto create_buffer_uav = [=]() {
        auto*               vk_buffer = static_cast<VulkanRHIBuffer*>(_resource);
        VulkanRHIBufferUAV* vk_uav    = MoerNew(VulkanRHIBufferUAV)(m_device, _resource, _view_info);

        VkBufferViewCreateInfo buffer_view_create_info{};
        buffer_view_create_info.sType  = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
        buffer_view_create_info.pNext  = nullptr;
        buffer_view_create_info.flags  = 0;
        buffer_view_create_info.buffer = vk_buffer->GetHandle();
        buffer_view_create_info.format = VulkanEnumTranslator::METoVKFormat(_view_info.buffer.uav.format);
        buffer_view_create_info.offset = _view_info.buffer.uav.byte_offset;
        buffer_view_create_info.range  = _view_info.buffer.uav.stride * _view_info.buffer.uav.num_elements;

        VK_CHECK_RESULT(vkCreateBufferView(m_device->GetDevice(), &buffer_view_create_info, nullptr, &vk_uav->m_view));

        return RHIUAVRef(vk_uav);
    };

    auto create_texture_uav = [=]() {
        auto* vk_texture = static_cast<VulkanRHITexture*>(_resource);

        VK_CHECK_NULLPTR(vk_texture, "RHICreateUnorderedAccessView: resource to be viewed is nullptr!", return RHIUAVRef{});

        VulkanRHITextureUAV* vk_uav = MoerNew(VulkanRHITextureUAV)(m_device, _resource, _view_info);

        VkImageViewCreateInfo image_view_create_info{};
        image_view_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        image_view_create_info.pNext = nullptr;
        image_view_create_info.flags = 0;

        image_view_create_info.image    = vk_texture->GetHandle();
        image_view_create_info.viewType = VulkanEnumTranslator::METoVKImageViewType(_view_info.texture.uav.dimension);
        image_view_create_info.format   = _view_info.texture.uav.format == PF_UNDEFINED ? VulkanEnumTranslator::METoVKFormat(vk_texture->GetUAVFormat()) : VulkanEnumTranslator::METoVKFormat(_view_info.texture.uav.format);
        assert(image_view_create_info.format != VK_FORMAT_UNDEFINED && "RHICreateUnorderedAccessView: format is undefined!");

        image_view_create_info.components                      = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
        image_view_create_info.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;// MARK...
        image_view_create_info.subresourceRange.baseMipLevel   = _view_info.texture.uav.mip_min;
        image_view_create_info.subresourceRange.levelCount     = _view_info.texture.uav.mip_num;
        image_view_create_info.subresourceRange.baseArrayLayer = _view_info.texture.uav.array_min;
        image_view_create_info.subresourceRange.layerCount     = _view_info.texture.uav.array_num;

        VK_CHECK_RESULT(vkCreateImageView(m_device->GetDevice(), &image_view_create_info, nullptr, &vk_uav->m_view));

        return RHIUAVRef(vk_uav);
    };

    if (_view_info.IsBuffer()) {
        return create_buffer_uav();
    }
    return create_texture_uav();
}

RHICommandQueue* VulkanRHIImpl::RHICreateCommandQueue(ECommandQueueType _type) {
    return MoerNew(VulkanRHICommandQueue(m_device, _type));
}

// RHIGraphicsCommandList* VulkanRHIImpl::CreateGraphicsCommandList(RHIGraphicsPipelineState* _initial_state) {
//     // todo: need to be more detailed.
//     return MoerNew(VulkanRHIGraphicsCommandList(m_device, m_device->GetDefaultCommandPool(), VK_COMMAND_BUFFER_LEVEL_PRIMARY));
// }

RHIGraphicsCommandList* VulkanRHIImpl::RHICreateGraphicsCommandList(RHICommandAllocator* _allocator, RHIGraphicsPipelineState* _initial_state) {
    auto* vk_allocator = static_cast<VulkanCommandAllocator*>(_allocator);
    VK_CHECK_NULLPTR(vk_allocator, "RHICreateGraphicsCommandList: allocator is nullptr!", return nullptr);

    return MoerNew(VulkanRHIGraphicsCommandList(m_device, vk_allocator->GetHandle(ECommandListType::GRAPHICS), VK_COMMAND_BUFFER_LEVEL_PRIMARY));
}

// RHIComputeCommandList* VulkanRHIImpl::CreateComputeCommandList(RHIComputePipelineState* _initial_state) {
//     return nullptr;
// }
RHICopyCommandList* VulkanRHIImpl::RHICreateCopyCommandList(RHICommandAllocator* _allocator) {
    auto* vk_allocator = static_cast<VulkanCommandAllocator*>(_allocator);
    VK_CHECK_NULLPTR(vk_allocator, "RHICreateCopyCommandList: allocator is nullptr!", return nullptr);

    return MoerNew(VulkanRHICopyCommandList(m_device, vk_allocator->GetHandle(ECommandListType::COPY), VK_COMMAND_BUFFER_LEVEL_PRIMARY));
}

void VulkanRHIImpl::RHISetBatchedShaderParametersInner(RHIResource* _pso, const RHIBatchedShaderParameters& _batched_params, bool b_update_constant) {
    const auto* vk_pso = static_cast<VulkanRHIGraphicsPipelineState*>(_pso);
    VK_CHECK_NULLPTR(vk_pso, "SetBatchedShaderParameter: graphics pipeline state is nullptr!", return);
    // resources
    const auto& descriptor_binding_infos = vk_pso->GetDescriptorSetsLayout()->GetDescriptorBindingInfos();
    auto*       resource_cache           = vk_pso->GetPipelineResourceCache();
    const auto& descriptor_sets          = resource_cache->GetDescriptorSets();
    auto&       writers                  = resource_cache->GetWriters();

    for (const auto& params : _batched_params.GetResourceParameters()) {
        const auto  type         = params.resource->GetResourceType();
        const auto& binding_info = descriptor_binding_infos.at(params.space).at(params.slot);
        if (type == ERHIResourceType::RRT_SAMPLER) {
            // sampler
            auto* vk_sampler = static_cast<VulkanRHISampler*>(params.resource);
            VK_CHECK_NULLPTR(vk_sampler, "SetBatchedShaderParameter: sampler is not supported yet!", continue);
            writers[params.space].WriteSampler(
                params.space,
                params.slot,
                {vk_sampler->GetHandle(), VK_NULL_HANDLE, vk_sampler->GetImageLayout()},
                binding_info.count,
                binding_info.type);
        } else {
            // view
            auto* view = static_cast<RHIView*>(params.resource);
            VK_CHECK_NULLPTR(view, "SetBatchedShaderParameter: resource view is nullptr!", continue);

            if (view->IsBuffer()) {
                auto* buffer = static_cast<VulkanRHIBuffer*>(view->GetBuffer());
                writers[params.space].WriteBuffer(
                    params.space,
                    params.slot,
                    {buffer->GetHandle(), 0, buffer->GetInfo().size},
                    binding_info.count,
                    binding_info.type);
            } else if (view->IsSRV()) {
                // MARK: 如何获取Sampler, 参数填充不足
                auto* texture_srv = static_cast<VulkanRHITextureSRV*>(view)->GetView();
                writers[params.space].WriteImage(
                    params.space,
                    params.slot,
                    {VK_NULL_HANDLE, texture_srv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                    binding_info.count,
                    binding_info.type);
            } else if (view->IsUAV()) {
                auto* texture_uav = static_cast<VulkanRHITextureUAV*>(view)->GetView();
                writers[params.space].WriteImage(
                    params.space,
                    params.slot,
                    {VK_NULL_HANDLE, texture_uav, VK_IMAGE_LAYOUT_GENERAL},
                    binding_info.count,
                    binding_info.type);
            }
        }
    }

    // cache push constants
    const auto& push_constants = _batched_params.GetConstantParameters();
    if (!b_update_constant) return;
    for (const auto& params : push_constants) {
        vk_pso->m_pipeline_state_cache->AddConstantToPush({VulkanEnumTranslator::METoVKShaderStageFlags(params.shader_type),
                                                           (uint32_t)params.size_in_32bit * 4,
                                                           params.byte_offset_in_raw_data,
                                                           std::move(_batched_params.GetRawData())});
    }
}

RHICommandAllocator* VulkanRHIImpl::RHIGetCurrentCommandAllocator() {
    RHICommandAllocator* allocator = m_device->GetCurrentCommandAllocator();
    VK_CHECK_NULLPTR(allocator, "RHIGetCurrentCommandAllocator: current command allocator is nullptr!", return nullptr);
    return m_device->GetCurrentCommandAllocator();
}

#pragma endregion

void VulkanRHIImpl::InitSurface(Moer::WindowHandle* _handle) {
    Moer::WindowContext::CreateVulkanSurface(m_instance, _handle, nullptr, &m_surface);
}

void VulkanRHIImpl::InitVulkan() {
    DeviceInitializer initializer;
    initializer.instance           = m_instance;
    initializer.surface            = m_surface;
    initializer.api_version        = VK_API_VERSION_1_3;
    initializer.enabled_features   = VulkanDeviceFeature::GetMESupportedDeviceFeatures(initializer.api_version);
    initializer.enabled_extensions = VulkanDeviceExtension::GetMESupportedDeviceExtensions();

    m_device = new VulkanDevice();
    m_device->Init(initializer);
    m_device->InitMemoryAllocator(m_instance);
    RHIViewportInitializer viewport_init{};
    viewport_init.window_handle = Moer::WindowContext::GetMainWindow();
    auto viewport               = RHICreateViewport(viewport_init);
    m_main_viewport             = (VulkanViewport*)viewport.Get();

    m_main_viewport->AddRef();
    // VulkanSwapChain* swap_chain = new VulkanSwapChain();
    // swap_chain->Connect(m_instance, m_surface, m_device);
    // uint32_t width, height;

    // swap_chain->Init(&width, &height, Moer::ConfigManager::GetInstance().GetInitConfig().editor_vsync);
    // m_main_viewport = new VulkanViewport(swap_chain, max_frame_in_flight);

    //init command allocator
}

#pragma region vulkan functions

void VulkanRHIImpl::CreateInstance() {
    VkApplicationInfo application_info{};
    application_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    // application_info.pApplicationName = MACRO_STR(__ENGINE_NAME__);
    // application_info.applicationVersion = VK_MAKE_VERSION(PROJECT_VERSION_MAJOR, PROJECT_VERSION_MINOR, PROJECT_VERSION_PATCH);
    application_info.pEngineName   = MACRO_STR(__ENGINE_NAME__);
    application_info.engineVersion = VK_MAKE_VERSION(PROJECT_VERSION_MAJOR, PROJECT_VERSION_MINOR, PROJECT_VERSION_PATCH);
    application_info.apiVersion    = VK_API_VERSION_1_3;

    m_instance_extensions         = VulkanInstanceExtension::GetDriverSupportedInstanceExtensionNames();
    m_enabled_instance_extensions = VulkanInstanceExtension::GetMESupportedInstanceExtensions();

    bool extension_supported   = CheckEnabledExtensions();
    bool debug_utils_available = std::find(m_enabled_instance_extensions.begin(), m_enabled_instance_extensions.end(), VK_EXT_DEBUG_UTILS_EXTENSION_NAME) != m_enabled_instance_extensions.end();

    VkInstanceCreateInfo instance_create_info{};
    instance_create_info.sType            = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_create_info.pNext            = nullptr;
    instance_create_info.flags            = 0;
    instance_create_info.pApplicationInfo = &application_info;

    auto n = m_enabled_instance_extensions.size();

    Moer::Array<const char*> r_extensions(n, nullptr);
    for (size_t i = 0; i < n; ++i) {
        r_extensions[i] = m_enabled_instance_extensions[i].c_str();
    }
    instance_create_info.enabledExtensionCount   = n;
    instance_create_info.ppEnabledExtensionNames = r_extensions.data();

    VkDebugUtilsMessengerCreateInfoEXT debug_create_info{};

    const char* validation_layer_name = "VK_LAYER_KHRONOS_validation";

    if (CheckValidationLayer(validation_layer_name)) {
        instance_create_info.enabledLayerCount   = 1;
        instance_create_info.ppEnabledLayerNames = &validation_layer_name;
        Moer::RHI::Vulkan::Debug::PopulateDebugMessengerCreateInfo(debug_create_info);
        instance_create_info.pNext = &debug_create_info;
    } else {
        instance_create_info.enabledLayerCount   = 0;
        instance_create_info.ppEnabledLayerNames = nullptr;
    }

    VK_CHECK_RESULT(vkCreateInstance(&instance_create_info, nullptr, &m_instance))

    Moer::RHI::Vulkan::Debug::SetupDebugUtilsMessengerEXT(m_instance);

    if (debug_utils_available) {
        Moer::RHI::Vulkan::DebugUtils::Setup(m_instance);
    }
}

#pragma endregion

#pragma region helper functions

bool VulkanRHIImpl::CheckValidationLayer(const std::string& layer_name) {
    uint32_t instance_layer_count = 0;
    vkEnumerateInstanceLayerProperties(&instance_layer_count, nullptr);
    Moer::Array<VkLayerProperties> instance_layer_properties(instance_layer_count);
    vkEnumerateInstanceLayerProperties(&instance_layer_count, instance_layer_properties.data());
    bool validation_layer_present = false;

    for (auto layer_property : instance_layer_properties) {
        if (layer_name == layer_property.layerName) {
            validation_layer_present = true;
            break;
        }
    }

    // return validation_layer_present;
    return false;
    //MARK_TEST
}

bool VulkanRHIImpl::CheckEnabledExtensions() {
    if (!m_enabled_instance_extensions.empty()) {
        for (const auto& extension : m_enabled_instance_extensions) {
            if (std::find(m_instance_extensions.begin(), m_instance_extensions.end(), extension) == m_instance_extensions.end()) {
                VkUtil::ExitFatal("Enabled instance extension '" + std::string(extension) + "' is not supported!", -1);
                return false;
            }
        }
    }
    return true;
}

VkCommandBuffer VulkanRHIImpl::BeginSingleTimeCommands(VkCommandPool _pool) {
    VkCommandBufferAllocateInfo alloc_info{};
    alloc_info.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.pNext              = nullptr;
    alloc_info.commandPool        = _pool;
    alloc_info.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = 1;

    VkCommandBuffer command_buffer;
    VK_CHECK_RESULT(vkAllocateCommandBuffers(m_device->GetDevice(), &alloc_info, &command_buffer));

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.pNext = nullptr;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vkBeginCommandBuffer(command_buffer, &begin_info);

    return command_buffer;
}

void VulkanRHIImpl::EndSingleTimeCommands(VkCommandBuffer _command_buffer, VkCommandPool _pool, VkQueue _queue) {
    vkEndCommandBuffer(_command_buffer);

    VkSubmitInfo submit_info{};
    submit_info.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.pNext                = nullptr;
    submit_info.waitSemaphoreCount   = 0;
    submit_info.pWaitSemaphores      = nullptr;
    submit_info.pWaitDstStageMask    = nullptr;
    submit_info.commandBufferCount   = 1;
    submit_info.pCommandBuffers      = &_command_buffer;
    submit_info.signalSemaphoreCount = 0;
    submit_info.pSignalSemaphores    = nullptr;

    vkQueueSubmit(_queue, 1, &submit_info, VK_NULL_HANDLE);
    vkQueueWaitIdle(_queue);

    vkFreeCommandBuffers(m_device->GetDevice(), _pool, 1, &_command_buffer);
}

#pragma endregion

#pragma region viewport
RHIViewport*   VulkanRHIImpl::RHIGetMainViewport() {
    return static_cast<RHIViewport*>(m_main_viewport);
}
//create external viewport
RHIViewportRef VulkanRHIImpl::RHICreateViewport(const RHIViewportInitializer& _init) {
    VulkanSwapChain* swapchain = new VulkanSwapChain();
    uint32_t         width, height;
    VkSurfaceKHR     surface;
    Moer::WindowContext::CreateVulkanSurface(m_instance, _init.window_handle, nullptr, &surface);
    swapchain->Connect(m_instance, surface, m_device);
    swapchain->Init(&width, &height, _init.b_vsync);

    VulkanViewport* viewport = new VulkanViewport(swapchain, max_frame_in_flight);

    return viewport;
}
void VulkanRHIImpl::RHIResizeViewport(RHIViewport* _viewport, Extent2D _size, bool _b_full_screen, EPixelFormat _format) {
    assert(_viewport != nullptr && "Passing invalid viewport");
    VulkanViewport* vk_viewport = static_cast<VulkanViewport*>(_viewport);

    auto* swapchain = (VulkanSwapChain*)vk_viewport->GetNativeSwapchain();

    vk_viewport->OnResize(_size);
}
RHIViewportNextBackBufferInfo VulkanRHIImpl::RHIGetNextFrameViewportBufferInfo(RHIViewport* _viewport) {
    assert(_viewport != nullptr && "Passing invalid viewport");
    VulkanViewport* vk_viewport = static_cast<VulkanViewport*>(_viewport);

    return vk_viewport->GetNextFrameBackBufferInfo();
}
RHIUAV* VulkanRHIImpl::RHIGetViewportBackBufferUAV(RHIViewport* _viewport, uint32_t index) {
    assert(_viewport != nullptr && "Passing invalid viewport");
    VulkanViewport* vk_viewport = static_cast<VulkanViewport*>(_viewport);
    if (index == UINT32_MAX) {
        LOG_WARNING("Not valid viewport back buffer index");
        return nullptr;
    }
    VulkanRHITextureUAV* uav = vk_viewport->GetCurrentBackBuffer(index);
    return static_cast<RHIUAV*>(uav);
}
void VulkanRHIImpl::RHIPresentViewport(RHIViewport* _viewport, RHIFence* _render_end_fence) {
    assert(_viewport != nullptr && "Passing invalid viewport");
    VulkanViewport* vk_viewport = static_cast<VulkanViewport*>(_viewport);

    uint32_t value = _render_end_fence->GetValue();

    vk_viewport->Present(_render_end_fence);
}
#pragma endregion
