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

#include <algorithm>
#include <cstdint>
#include <string>
#include <type_traits>

namespace VkUtil = Moer::RHI::Vulkan::Util;

VulkanRHIImpl::VulkanRHIImpl()
    : m_instance(VK_NULL_HANDLE), m_surface(VK_NULL_HANDLE),
      m_device(nullptr), m_main_viewport(nullptr) {
    LOG_INFO("Built with Vulkan header version {0:d}.{1:d}.{2:d}", VK_API_VERSION_MAJOR(VK_HEADER_VERSION_COMPLETE), VK_API_VERSION_MINOR(VK_HEADER_VERSION_COMPLETE), VK_API_VERSION_PATCH(VK_HEADER_VERSION_COMPLETE));
    m_rhi_info.rhi_type = ERHIType::Vulkan;
}

void VulkanRHIImpl::Initialize(const RHIInitInfo& _init) {
    //todo: need more elegant way
    m_rhi_info.max_frame_in_flight = _init.max_frame_in_flight;
    m_rhi_info.ray_tracing         = _init.ray_tracing;

    LOG_INFO("raytraing: {}", _init.ray_tracing);

    CreateInstance();
    InitSurface(Moer::WindowContext::GetMainWindow());
    InitVulkan();
}

void VulkanRHIImpl::PostInit() {
    LOG_INFO("VulkanRHIImpl::PostInit()");
}
#include "RenderThread.h"
void VulkanRHIImpl::ShutDown() {
    vkDeviceWaitIdle(m_device->GetDevice());
    Moer::RenderThreadFence fence;
    fence.BeginFence();
    fence.Wait();
    fence.BeginFence();
    fence.Wait();
    CHECK_AND_DELETE(m_main_viewport);
    CHECK_AND_DELETE(m_device);
}

#pragma region resources creation
RHISamplerRef  VulkanRHIImpl::RHICreateSampler(const RHISamplerCreateInfo& _initializer) {
    VulkanRHISampler* vk_sampler = MoerNew(VulkanRHISampler)();
    vk_sampler->GenerateSamplerFromInitializer(m_device, _initializer);

    return RHISamplerRef(vk_sampler);
}

RHIVertexShaderRef VulkanRHIImpl::RHICreateVertexShader(const class ShaderCodeEntry* code_entry, const Shader* shader) {
    auto* vk_shader            = MoerNew(VulkanRHIVertexShader)(shader);
    vk_shader->m_shader_module = VkUtil::CreateShaderModule(code_entry->code, m_device->GetDevice());

    return RHIVertexShaderRef(vk_shader);
}

RHIFragmentShaderRef VulkanRHIImpl::RHICreateFragmentShader(const class ShaderCodeEntry* code_entry, const Shader* shader) {
    auto* vk_shader            = MoerNew(VulkanRHIFragmentShader)(shader);
    vk_shader->m_shader_module = VkUtil::CreateShaderModule(code_entry->code, m_device->GetDevice());

    return RHIFragmentShaderRef(vk_shader);
}

RHIGeometryShaderRef VulkanRHIImpl::RHICreateGeometryShader(const class ShaderCodeEntry* code_entry, const Shader* shader) {
    auto* vk_shader            = MoerNew(VulkanRHIGeometryShader)(shader);
    vk_shader->m_shader_module = VkUtil::CreateShaderModule(code_entry->code, m_device->GetDevice());

    return RHIGeometryShaderRef(vk_shader);
}

RHIMeshShaderRef VulkanRHIImpl::RHICreateMeshShader(const class ShaderCodeEntry* code_entry, const Shader* shader) {
    auto* vk_shader            = MoerNew(VulkanRHIMeshShader)(shader);
    vk_shader->m_shader_module = VkUtil::CreateShaderModule(code_entry->code, m_device->GetDevice());

    return RHIMeshShaderRef(vk_shader);
}

RHIAmplificationShaderRef VulkanRHIImpl::RHICreateAmplificationShader(const class ShaderCodeEntry* code_entry, const Shader* shader) {
    auto* vk_shader            = MoerNew(VulkanRHIAmplificationShader)(shader);
    vk_shader->m_shader_module = VkUtil::CreateShaderModule(code_entry->code, m_device->GetDevice());

    return RHIAmplificationShaderRef(vk_shader);
}

RHIComputeShaderRef VulkanRHIImpl::RHICreateComputeShader(const class ShaderCodeEntry* code_entry, const Shader* shader) {
    auto* vk_shader            = MoerNew(VulkanRHIComputeShader)(shader);
    vk_shader->m_shader_module = VkUtil::CreateShaderModule(code_entry->code, m_device->GetDevice());

    return RHIComputeShaderRef(vk_shader);
}

RHIRayGenShaderRef VulkanRHIImpl::RHICreateRayGenShader(const class ShaderCodeEntry* code_entry, const Shader* shader) {
    auto* vk_shader            = new VulkanRHIRayGenShader(shader);
    vk_shader->m_shader_module = VkUtil::CreateShaderModule(code_entry->code, m_device->GetDevice());
    return RHIRayGenShaderRef(vk_shader);
}

RHIRayMissShaderRef VulkanRHIImpl::RHICreateRayMissShader(const class ShaderCodeEntry* code_entry, const Shader* shader) {
    auto* vk_shader            = new VulkanRHIRayMissShader(shader);
    vk_shader->m_shader_module = VkUtil::CreateShaderModule(code_entry->code, m_device->GetDevice());
    return RHIRayMissShaderRef(vk_shader);
}

RHIRayClosestHitShaderRef VulkanRHIImpl::RHICreateRayClosestHitShader(const class ShaderCodeEntry* code_entry, const Shader* shader) {
    auto* vk_shader            = new VulkanRHIRayClosestHitShader(shader);
    vk_shader->m_shader_module = VkUtil::CreateShaderModule(code_entry->code, m_device->GetDevice());
    return RHIRayClosestHitShaderRef(vk_shader);
}

RHIRayCallableShaderRef VulkanRHIImpl::RHICreateRayCallableShader(const class ShaderCodeEntry* code_entry, const Shader* shader) {
    auto* vk_shader            = new VulkanRHIRayCallableShader(shader);
    vk_shader->m_shader_module = VkUtil::CreateShaderModule(code_entry->code, m_device->GetDevice());
    return RHIRayCallableShaderRef(vk_shader);
}

RHIRayIntersectionShaderRef VulkanRHIImpl::RHICreateRayIntersectionShader(const class ShaderCodeEntry* code_entry, const Shader* shader) {
    auto* vk_shader            = new VulkanRHIRayIntersectionShader(shader);
    vk_shader->m_shader_module = VkUtil::CreateShaderModule(code_entry->code, m_device->GetDevice());
    return RHIRayIntersectionShaderRef(vk_shader);
}

RHIRayAnyhitShaderRef VulkanRHIImpl::RHICreateRayAnyhitShader(const class ShaderCodeEntry* code_entry, const Shader* shader) {
    auto* vk_shader            = new VulkanRHIRayAnyhitShader(shader);
    vk_shader->m_shader_module = VkUtil::CreateShaderModule(code_entry->code, m_device->GetDevice());
    return RHIRayAnyhitShaderRef(vk_shader);
}

RHIShaderLibraryRef VulkanRHIImpl::RHICreateShaderLibrary(EShaderPlatform _platform, const std::string& _file_path, const std::string& name) { return RHIShaderLibraryRef{}; }

RHIFenceRef VulkanRHIImpl::RHICreateFence(const RHIFenceCreateInfo& _info) {

    VulkanRHIFence* vk_fence = MoerNew(VulkanRHIFence)(m_device, _info.usage);

    return RHIFenceRef(vk_fence);
}

// RHIGraphicsPipelineStateRef VulkanRHIImpl::RHICreateGraphicsPipelineState(const RHIGraphicsPipelineStateInfo& _init) {
//     VulkanRHIGraphicsPipelineState* vk_pso = MoerNew(VulkanRHIGraphicsPipelineState)();

//     uint32_t attachment_count = _init.CalcValidColorAttachmentCount();

//     // rendering create info
//     VkPipelineRenderingCreateInfo rendering_create_info{};
//     rendering_create_info.sType                = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
//     rendering_create_info.pNext                = nullptr;
//     rendering_create_info.viewMask             = 0;
//     rendering_create_info.colorAttachmentCount = attachment_count;
//     Moer::Array<VkFormat> color_attachment_formats(attachment_count);
//     for (int i = 0; i < attachment_count; ++i) {
//         color_attachment_formats[i] = VkFormat(_init.color_attachment_formats[i]);
//     }
//     rendering_create_info.pColorAttachmentFormats = color_attachment_formats.data();
//     rendering_create_info.depthAttachmentFormat   = VulkanEnumTranslator::METoVKFormat(_init.depth_stencil_format);
//     rendering_create_info.stencilAttachmentFormat = VulkanEnumTranslator::METoVKFormat(_init.depth_stencil_format);

//     // color blend state
//     VkPipelineColorBlendStateCreateInfo color_blend_state{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};

//     auto* vk_blend_state = static_cast<VulkanRHIBlendState*>(_init.blend_state.Get());
//     if (!vk_blend_state) LOG_CRITICAL("RHICreateGraphicsPipelineState: Blend state is nullptr!");
//     color_blend_state.logicOp         = VK_LOGIC_OP_COPY;
//     color_blend_state.logicOpEnable   = VK_FALSE;
//     color_blend_state.attachmentCount = attachment_count;
//     color_blend_state.pAttachments    = vk_blend_state->GetAttachments();
//     // shader stage
//     auto shader_stages = VulkanRHIGraphicsPipelineState::METoVKShaderStageCreateInfo(_init.shader_stage);

//     // vertex input state
//     auto vertex_input_state = VulkanRHIGraphicsPipelineState::METoVKVertexInputStateCreateInfo(_init.shader_stage.p_vertex_input_state);

//     // input assembly
//     VkPipelineInputAssemblyStateCreateInfo input_assembly_state{};
//     input_assembly_state.sType                  = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
//     input_assembly_state.pNext                  = nullptr;
//     input_assembly_state.flags                  = 0;
//     input_assembly_state.topology               = VulkanEnumTranslator::METoVKPrimitiveTopology(_init.primitive_topology);
//     input_assembly_state.primitiveRestartEnable = VK_FALSE;

//     // viewport state
//     VkPipelineViewportStateCreateInfo viewport_state{};
//     viewport_state.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
//     viewport_state.pNext         = nullptr;
//     viewport_state.flags         = 0;
//     viewport_state.viewportCount = _init.multi_view_count;
//     viewport_state.scissorCount  = _init.multi_view_count;

// #define CHECK_AND_SET(ptr, state, msg) \
//     if (ptr)                           \
//         state = ptr->GetHandle();      \
//     else                               \
//         LOG_CRITICAL(msg);

//     // rasterization state
//     VkPipelineRasterizationStateCreateInfo rasterization_state{};

//     auto* vk_rasterizer_state = static_cast<VulkanRHIRasterizationState*>(_init.rasterizer_state.Get());
//     CHECK_AND_SET(vk_rasterizer_state, rasterization_state, "RHICreateGraphicsPipelineState: rasterization state is nullptr!");

//     // multisample state
//     VkPipelineMultisampleStateCreateInfo multisample_state{};

//     auto* vk_multisample_state = static_cast<VulkanRHIMultisampleState*>(_init.multisample_state.Get());
//     CHECK_AND_SET(vk_multisample_state, multisample_state, "RHICreateGraphicsPipelineState: multisample state is nullptr!");

//     // depth stencil state
//     VkPipelineDepthStencilStateCreateInfo depth_stencil_state{};

//     auto* vk_depth_stencil_state = static_cast<VulkanRHIDepthStencilState*>(_init.depth_stencil_state.Get());
//     CHECK_AND_SET(vk_depth_stencil_state, depth_stencil_state, "RHICreateGraphicsPipelineState: depth stencil state is nullptr!");

// #undef CHECK_AND_SET

//     // dynamic state
//     Moer::StaticArray<VkDynamicState, 2> states = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
//     VkPipelineDynamicStateCreateInfo     dynamic_state{};
//     dynamic_state.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
//     dynamic_state.pNext             = nullptr;
//     dynamic_state.flags             = 0;
//     dynamic_state.dynamicStateCount = states.size();
//     dynamic_state.pDynamicStates    = states.data();

//     // pipeline layout
//     auto shader_info_list = VulkanRHIGraphicsPipelineState::GetShaderInfoList(_init.shader_stage);// MARK...

//     Moer::Array<TDescriptorSetLayoutBindingArray> layout_mappings;
//     Moer::Array<VkPushConstantRange>      push_constant_ranges;

//     // find max set index
//     int8_t max_set = -1;
//     for (const auto* meta_shader : shader_info_list) {
//         auto layout_infos = meta_shader->GetRootParametersLayoutInfo().GetLayoutInfos();
//         for (const auto& info : layout_infos) {
//             max_set = std::max(max_set, info.space);
//         }
//     }
//     layout_mappings.resize(max_set + 1, {});

//     // construct layout mappings
//     for (const auto* meta_shader : shader_info_list) {
//         auto layout_infos   = meta_shader->GetRootParametersLayoutInfo().GetLayoutInfos();
//         auto constant_infos = meta_shader->GetRootParametersLayoutInfo().GetConstantsInfo();

//         for (const auto& info : layout_infos) {
//             VkDescriptorSetLayoutBinding binding{};
//             binding.binding         = info.slot;
//             binding.descriptorType  = VulkanEnumTranslator::METoVKDescriptorType(info.type, info.resource_type);
//             binding.descriptorCount = 1;
//             binding.stageFlags |= VulkanEnumTranslator::METoVKShaderStageFlags(meta_shader->GetShaderType());
//             binding.pImmutableSamplers = nullptr;

//             layout_mappings[info.space].second.push_back(std::move(binding));
//         }

//         // constants
//         for (const auto& info : constant_infos) {
//             VkPushConstantRange range{};
//             range.stageFlags |= VulkanEnumTranslator::METoVKShaderStageFlags(meta_shader->GetShaderType());
//             range.offset = info.offset;
//             range.size   = info.stride;
//             push_constant_ranges.push_back(range);
//         }
//     }

//     // generate descriptor set layouts
//     vk_pso->CreateResourceCache();
//     vk_pso->GenerateDescriptorSetLayouts(m_device, layout_mappings);

//     auto layouts = vk_pso->m_descriptor_sets_layout->GetLayouts();
//     // create pipeline layout
//     VkPipelineLayoutCreateInfo pipeline_layout_create_info{};
//     pipeline_layout_create_info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
//     pipeline_layout_create_info.pNext                  = nullptr;
//     pipeline_layout_create_info.flags                  = 0;
//     pipeline_layout_create_info.setLayoutCount         = layouts.size();
//     pipeline_layout_create_info.pSetLayouts            = layouts.data();
//     pipeline_layout_create_info.pushConstantRangeCount = push_constant_ranges.size();
//     pipeline_layout_create_info.pPushConstantRanges    = push_constant_ranges.data();

//     vkCreatePipelineLayout(m_device->GetDevice(), &pipeline_layout_create_info, nullptr, &vk_pso->m_pipeline_layout);

//     VkGraphicsPipelineCreateInfo pipeline_create_info{};
//     pipeline_create_info.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
//     pipeline_create_info.pNext               = &rendering_create_info;
//     pipeline_create_info.flags               = 0;
//     pipeline_create_info.stageCount          = shader_stages.size();
//     pipeline_create_info.pStages             = shader_stages.data();
//     pipeline_create_info.pVertexInputState   = &vertex_input_state;
//     pipeline_create_info.pInputAssemblyState = &input_assembly_state;
//     pipeline_create_info.pTessellationState  = nullptr;
//     pipeline_create_info.pViewportState      = &viewport_state;
//     pipeline_create_info.pRasterizationState = &rasterization_state;
//     pipeline_create_info.pMultisampleState   = &multisample_state;
//     pipeline_create_info.pDepthStencilState  = &depth_stencil_state;
//     pipeline_create_info.pColorBlendState    = &color_blend_state;
//     pipeline_create_info.pDynamicState       = &dynamic_state;
//     pipeline_create_info.layout              = vk_pso->m_pipeline_layout;
//     pipeline_create_info.renderPass          = nullptr;
//     pipeline_create_info.subpass             = 0;
//     pipeline_create_info.basePipelineHandle  = nullptr;// MARK...
//     pipeline_create_info.basePipelineIndex   = -1;

//     VK_CHECK_RESULT(vkCreateGraphicsPipelines(m_device->GetDevice(), nullptr, 1, &pipeline_create_info, nullptr, &vk_pso->m_pipeline));

//     return RHIGraphicsPipelineStateRef(vk_pso);
// }
void PopulateVertexAttribute(
    Moer::Array<VkVertexInputBindingDescription>&   _bindings,
    Moer::Array<VkVertexInputAttributeDescription>& _attributes,
    const RHIVertexInputInfo&                       _info) {

    _bindings.resize(_info.vertex_elements.size());
    _attributes.resize(_info.vertex_elements.size());
    uint32_t max_binding = 0;
    for (uint32_t i = 0; i < _info.vertex_elements.size(); ++i) {
        const auto& element = _info.vertex_elements[i];
        if (element.format == EPixelFormat::PF_UNDEFINED) {
            break;
        }

        _attributes[i].location = element.attribute_index;
        _attributes[i].binding  = element.binding_index;
        _attributes[i].format   = VulkanEnumTranslator::METoVKFormat(element.format);
        _attributes[i].offset   = element.offset;

        max_binding                      = std::max(max_binding, static_cast<uint32_t>(element.binding_index));
        _bindings[max_binding].binding   = element.binding_index;
        _bindings[max_binding].stride    = element.stride;
        _bindings[max_binding].inputRate = VulkanEnumTranslator::METoVKVertexInputRate(element.input_rate);
    }
    _bindings.resize(max_binding + 1);
    _bindings.shrink_to_fit();
}
RHIGraphicsPipelineStateRef VulkanRHIImpl::RHICreateGraphicsPSO(RHIGraphicsPSOCreateInfo&& _init) {
    VulkanRHIGraphicsPipelineState* vk_pso = MoerNew(VulkanRHIGraphicsPipelineState)(m_device);

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
    rendering_create_info.depthAttachmentFormat   = VulkanEnumTranslator::METoVKFormat(_init.depth_stencil_format);
    rendering_create_info.stencilAttachmentFormat = VulkanEnumTranslator::METoVKFormat(_init.depth_stencil_format);

    auto to_vk_blend_attachment = [](const RHIBlendAttachmentInfo& _info) {
        VkPipelineColorBlendAttachmentState state{};
        state.blendEnable =
            (_info.color_blend_op != BO_ADD || _info.color_dst_blend_factor != BF_ZERO || _info.color_src_blend_factor != BF_ONE ||
             _info.alpha_blend_op != BO_ADD || _info.alpha_dst_blend_factor != BF_ZERO || _info.alpha_src_blend_factor != BF_ONE) ?
                VK_TRUE :
                VK_FALSE;
        state.srcColorBlendFactor = VulkanEnumTranslator::METoVKBlendFactor(_info.color_src_blend_factor);
        state.dstColorBlendFactor = VulkanEnumTranslator::METoVKBlendFactor(_info.color_dst_blend_factor);
        state.colorBlendOp        = VulkanEnumTranslator::METoVKBlendOp(_info.color_blend_op);
        state.srcAlphaBlendFactor = VulkanEnumTranslator::METoVKBlendFactor(_info.alpha_src_blend_factor);
        state.dstAlphaBlendFactor = VulkanEnumTranslator::METoVKBlendFactor(_info.alpha_dst_blend_factor);
        state.alphaBlendOp        = VulkanEnumTranslator::METoVKBlendOp(_info.alpha_blend_op);
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

    VkPipelineVertexInputStateCreateInfo           vertex_input_state{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    Moer::Array<VkVertexInputBindingDescription>   binding_descs;
    Moer::Array<VkVertexInputAttributeDescription> attribute_descs;

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

        PopulateVertexAttribute(binding_descs, attribute_descs, work_flow.vertex_input_info);
        vertex_input_state.vertexBindingDescriptionCount   = binding_descs.size();
        vertex_input_state.pVertexBindingDescriptions      = binding_descs.data();
        vertex_input_state.vertexAttributeDescriptionCount = attribute_descs.size();
        vertex_input_state.pVertexAttributeDescriptions    = attribute_descs.data();

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

    Moer::Array<TDescriptorSetLayoutBindingArray> descriptor_bindings;
    Moer::Array<VkPushConstantRange>              push_constant_ranges;

    // find max set index
    int8_t max_set = -1;
    for (const auto* meta_shader : shader_info_list) {
        auto layout_infos = meta_shader->GetRootParametersLayoutInfo().GetBindingInfo();
        for (const auto& info : layout_infos) {
            max_set = std::max(max_set, info.space);
        }
    }
    descriptor_bindings.resize(max_set + 1, {});

    // construct layout mappings
    for (const auto* meta_shader : shader_info_list) {
        auto binding_infos  = meta_shader->GetRootParametersLayoutInfo().GetBindingInfo();
        auto constant_infos = meta_shader->GetRootParametersLayoutInfo().GetConstantsInfo();

        for (const auto& info : binding_infos) {
            auto& bindings     = descriptor_bindings[info.space];
            auto  prev_binding = std::find_if(bindings.begin(), bindings.end(), [info](VkDescriptorSetLayoutBinding& binding) {
                return binding.binding == info.slot;
            });

            VkDescriptorSetLayoutBinding binding{};
            binding.binding         = info.slot;
            binding.descriptorType  = VulkanEnumTranslator::METoVKDescriptorType(info.type, info.resource_type);
            binding.descriptorCount = 1;// always descriptorCount = 1

            if (prev_binding != bindings.end()) {
                if (prev_binding->descriptorType != binding.descriptorType || prev_binding->descriptorCount != binding.descriptorCount) {
                    LOG_CRITICAL("RHICreateGraphicsPSO: descriptor type conflict!");
                }
                prev_binding->stageFlags |= VulkanEnumTranslator::METoVKShaderStageFlags(meta_shader->GetShaderType());
                continue;
            }

            binding.stageFlags |= VulkanEnumTranslator::METoVKShaderStageFlags(meta_shader->GetShaderType());
            binding.pImmutableSamplers = nullptr;

            descriptor_bindings[info.space].push_back(std::move(binding));
        }
        uint32_t constant_offset = 0;
        // constants
        for (const auto& info : constant_infos) {
            VkPushConstantRange range{};
            range.stageFlags |= VulkanEnumTranslator::METoVKShaderStageFlags(meta_shader->GetShaderType());
            range.offset = constant_offset;
            range.size   = info.stride;
            push_constant_ranges.push_back(range);
            constant_offset += info.stride;
        }
    }

    // init descriptor set layouts and pipeline resource cache
    vk_pso->InitDescriptorSetLayouts(descriptor_bindings);
    vk_pso->InitPipelineResourceCache(descriptor_bindings);

    const auto& layouts = vk_pso->GetDescriptorSetsLayout()->GetLayouts();
    // create pipeline layout
    VkPipelineLayoutCreateInfo pipeline_layout_create_info{};
    pipeline_layout_create_info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_create_info.pNext                  = nullptr;
    pipeline_layout_create_info.flags                  = 0;
    pipeline_layout_create_info.setLayoutCount         = layouts.size();
    pipeline_layout_create_info.pSetLayouts            = layouts.data();
    pipeline_layout_create_info.pushConstantRangeCount = push_constant_ranges.size();
    pipeline_layout_create_info.pPushConstantRanges    = push_constant_ranges.data();

    vk_pso->CreatePipelineLayout(pipeline_layout_create_info);
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
    pipeline_create_info.pMultisampleState   = &vk_multisample_state;
    pipeline_create_info.pDepthStencilState  = &vk_depth_stencil_state;
    pipeline_create_info.pColorBlendState    = &color_blend_state;
    pipeline_create_info.pDynamicState       = &dynamic_state;
    pipeline_create_info.layout              = vk_pso->GetPipelineLayout();
    pipeline_create_info.renderPass          = nullptr;
    pipeline_create_info.subpass             = 0;
    pipeline_create_info.basePipelineHandle  = nullptr;// MARK...
    pipeline_create_info.basePipelineIndex   = -1;

    vk_pso->CreateGraphicsPipeline(pipeline_create_info);

    return RHIGraphicsPipelineStateRef(vk_pso);
}

RHIComputePipelineStateRef VulkanRHIImpl::RHICreateComputePipelineState(RHIShader* _compute_shader) {
    VulkanRHIComputePipelineState* vk_pso = MoerNew(VulkanRHIComputePipelineState)(m_device);

    auto* vk_shader = static_cast<VulkanRHIComputeShader*>(_compute_shader);
    if (!vk_shader) LOG_CRITICAL("RHICreateComputePipelineState: Compute shader is nullptr!");

    VkPipelineShaderStageCreateInfo shader_stage{};
    shader_stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shader_stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    shader_stage.module = vk_shader->GetHandle();
    shader_stage.pName  = vk_shader->GetMetaShader()->GetShaderMetaType()->GetEntryPoint().data();

    Moer::Array<TDescriptorSetLayoutBindingArray> descriptor_bindings;
    Moer::Array<VkPushConstantRange>              push_constant_ranges;

    auto* meta_shader = vk_shader->GetMetaShader();
    // find max set index
    int8_t max_set       = -1;
    auto   binding_infos = meta_shader->GetRootParametersLayoutInfo().GetBindingInfo();
    for (const auto& info : binding_infos) {
        max_set = std::max(max_set, info.space);
    }
    descriptor_bindings.resize(max_set + 1, {});

    auto constant_infos = meta_shader->GetRootParametersLayoutInfo().GetConstantsInfo();

    for (const auto& info : binding_infos) {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding         = info.slot;
        binding.descriptorType  = VulkanEnumTranslator::METoVKDescriptorType(info.type, info.resource_type);
        binding.descriptorCount = info.num;
        binding.stageFlags |= VulkanEnumTranslator::METoVKShaderStageFlags(meta_shader->GetShaderType());
        binding.pImmutableSamplers = nullptr;

        descriptor_bindings[info.space].push_back(std::move(binding));
    }

    // constants
    for (const auto& info : constant_infos) {
        VkPushConstantRange range{};
        range.stageFlags |= VulkanEnumTranslator::METoVKShaderStageFlags(meta_shader->GetShaderType());
        range.offset = info.offset;
        range.size   = info.stride;

        push_constant_ranges.push_back(range);
    }

    vk_pso->InitDescriptorSetLayouts(descriptor_bindings);
    vk_pso->InitPipelineResourceCache(descriptor_bindings);

    const auto& layouts = vk_pso->GetDescriptorSetsLayout()->GetLayouts();

    VkPipelineLayoutCreateInfo pipeline_layout_create_info{};
    pipeline_layout_create_info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_create_info.pNext                  = nullptr;
    pipeline_layout_create_info.flags                  = 0;
    pipeline_layout_create_info.setLayoutCount         = layouts.size();
    pipeline_layout_create_info.pSetLayouts            = layouts.data();
    pipeline_layout_create_info.pushConstantRangeCount = push_constant_ranges.size();
    pipeline_layout_create_info.pPushConstantRanges    = push_constant_ranges.data();

    vk_pso->CreatePipelineLayout(pipeline_layout_create_info);

    VkComputePipelineCreateInfo pipeline_create_info{};
    pipeline_create_info.sType              = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline_create_info.pNext              = nullptr;
    pipeline_create_info.flags              = 0;
    pipeline_create_info.stage              = shader_stage;
    pipeline_create_info.layout             = vk_pso->GetPipelineLayout();
    pipeline_create_info.basePipelineHandle = nullptr;
    pipeline_create_info.basePipelineIndex  = -1;

    vk_pso->CreateComputePipeline(pipeline_create_info);

    return RHIComputePipelineStateRef(vk_pso);
}

RHIRayTracingPipelineStateRef VulkanRHIImpl::RHICreateRayTracingPipelineState(const RHIRayTracingPipelineStateInitializer& _init) {

    static auto vkCreateRayTracingPipelinesKHR       = reinterpret_cast<PFN_vkCreateRayTracingPipelinesKHR>(vkGetDeviceProcAddr(m_device->GetDevice(), "vkCreateRayTracingPipelinesKHR"));
    static auto vkGetRayTracingShaderGroupHandlesKHR = reinterpret_cast<PFN_vkGetRayTracingShaderGroupHandlesKHR>(vkGetDeviceProcAddr(m_device->GetDevice(), "vkGetRayTracingShaderGroupHandlesKHR"));
    VK_CHECK_NULLPTR(vkCreateRayTracingPipelinesKHR, "RHICreateRayTracingPipelineState: vkCreateRayTracingPipelinesKHR is nullptr", return RHIRayTracingPipelineStateRef{});
    VK_CHECK_NULLPTR(vkGetRayTracingShaderGroupHandlesKHR, "RHICreateRayTracingPipelineState: vkGetRayTracingShaderGroupHandlesKHR is nullptr", return RHIRayTracingPipelineStateRef{});

    // shader stage & shader groups & shader infos

    Moer::Array<const Shader*> shader_info_list;

    Moer::Array<VkRayTracingShaderGroupCreateInfoKHR> shader_groups;
    VkRayTracingShaderGroupCreateInfoKHR              shader_group_create_info{VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR};
    shader_group_create_info.pNext                           = nullptr;
    shader_group_create_info.pShaderGroupCaptureReplayHandle = nullptr;
    shader_group_create_info.generalShader                   = VK_SHADER_UNUSED_KHR;
    shader_group_create_info.closestHitShader                = VK_SHADER_UNUSED_KHR;
    shader_group_create_info.anyHitShader                    = VK_SHADER_UNUSED_KHR;
    shader_group_create_info.intersectionShader              = VK_SHADER_UNUSED_KHR;

    Moer::Array<VkPipelineShaderStageCreateInfo> shader_stages;
    VkPipelineShaderStageCreateInfo              shader_stage_create_info{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    shader_stage_create_info.pNext = nullptr;
    shader_stage_create_info.flags = 0;

    if (_init.ray_gen_shader) {
        auto* vk_shader = static_cast<VulkanRHIRayGenShader*>(_init.ray_gen_shader);
        VK_CHECK_NULLPTR(vk_shader, "init raytracingpipelinestate with null raygen shader", return RHIRayTracingPipelineStateRef());

        shader_info_list.push_back(vk_shader->GetMetaShader());

        shader_stage_create_info.stage               = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
        shader_stage_create_info.module              = vk_shader->GetHandle();
        shader_stage_create_info.pName               = "main";
        shader_stage_create_info.pSpecializationInfo = nullptr;
        shader_stages.push_back(shader_stage_create_info);

        shader_group_create_info.type          = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
        shader_group_create_info.generalShader = static_cast<uint32_t>(shader_stages.size() - 1);
        shader_groups.push_back(shader_group_create_info);
        shader_group_create_info.generalShader = VK_SHADER_UNUSED_KHR;
    }
    for (const auto& ray_miss_shader : _init.ray_miss_table) {
        auto* vk_shader = static_cast<VulkanRHIRayMissShader*>(ray_miss_shader);
        VK_CHECK_NULLPTR(vk_shader, "init raytracingpipelinestate with null raymiss shader", return RHIRayTracingPipelineStateRef());

        shader_info_list.push_back(vk_shader->GetMetaShader());

        shader_stage_create_info.stage               = VK_SHADER_STAGE_MISS_BIT_KHR;
        shader_stage_create_info.module              = vk_shader->GetHandle();
        shader_stage_create_info.pName               = "main";
        shader_stage_create_info.pSpecializationInfo = nullptr;
        shader_stages.push_back(shader_stage_create_info);

        shader_group_create_info.type          = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
        shader_group_create_info.generalShader = static_cast<uint32_t>(shader_stages.size() - 1);
        shader_groups.push_back(shader_group_create_info);
        shader_group_create_info.generalShader = VK_SHADER_UNUSED_KHR;
    }
    for (const auto& ray_hit_group : _init.ray_hit_table) {
        shader_group_create_info.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
        if (ray_hit_group.closesthit_shader) {
            auto* vk_shader = static_cast<VulkanRHIRayClosestHitShader*>(ray_hit_group.closesthit_shader);
            VK_CHECK_NULLPTR(vk_shader, "init raytracingpipelinestate with null closesthit shader", return RHIRayTracingPipelineStateRef());

            shader_info_list.push_back(vk_shader->GetMetaShader());

            shader_stage_create_info.stage               = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
            shader_stage_create_info.pName               = "main";
            shader_stage_create_info.module              = vk_shader->GetHandle();
            shader_stage_create_info.pSpecializationInfo = nullptr;
            shader_stages.push_back(shader_stage_create_info);

            shader_group_create_info.closestHitShader = static_cast<uint32_t>(shader_stages.size() - 1);
        }
        if (ray_hit_group.anyhit_shader) {
            auto* vk_shader = static_cast<VulkanRHIRayAnyhitShader*>(ray_hit_group.anyhit_shader);
            VK_CHECK_NULLPTR(vk_shader, "init raytracingpipelinestate with null anyhit shader", return RHIRayTracingPipelineStateRef());

            shader_info_list.push_back(vk_shader->GetMetaShader());

            shader_stage_create_info.stage               = VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
            shader_stage_create_info.pName               = "main";
            shader_stage_create_info.module              = vk_shader->GetHandle();
            shader_stage_create_info.pSpecializationInfo = nullptr;
            shader_stages.push_back(shader_stage_create_info);

            shader_group_create_info.anyHitShader = static_cast<uint32_t>(shader_stages.size() - 1);
        }
        if (ray_hit_group.intersection_shader) {
            auto* vk_shader = static_cast<VulkanRHIRayIntersectionShader*>(ray_hit_group.intersection_shader);
            VK_CHECK_NULLPTR(vk_shader, "init raytracingpipelinestate with null intersection shader", return RHIRayTracingPipelineStateRef());
            shader_stage_create_info.stage = VK_SHADER_STAGE_INTERSECTION_BIT_KHR;

            shader_info_list.push_back(vk_shader->GetMetaShader());

            shader_stage_create_info.pName               = "main";
            shader_stage_create_info.module              = vk_shader->GetHandle();
            shader_stage_create_info.pSpecializationInfo = nullptr;
            shader_stages.push_back(shader_stage_create_info);

            shader_group_create_info.type               = VK_RAY_TRACING_SHADER_GROUP_TYPE_PROCEDURAL_HIT_GROUP_KHR;
            shader_group_create_info.intersectionShader = static_cast<uint32_t>(shader_stages.size() - 1);
        }
        shader_groups.push_back(shader_group_create_info);
        shader_group_create_info.closestHitShader = shader_group_create_info.anyHitShader = shader_group_create_info.intersectionShader = VK_SHADER_UNUSED_KHR;
    }
    for (const auto& ray_callable_shader : _init.ray_callable_table) {
        auto* vk_shader = static_cast<VulkanRHIRayCallableShader*>(ray_callable_shader);
        VK_CHECK_NULLPTR(vk_shader, "init raytracingpipelinestate with null raycallable shader", return RHIRayTracingPipelineStateRef());

        shader_info_list.push_back(vk_shader->GetMetaShader());

        shader_stage_create_info.stage               = VK_SHADER_STAGE_CALLABLE_BIT_KHR;
        shader_stage_create_info.module              = vk_shader->GetHandle();
        shader_stage_create_info.pName               = "main";
        shader_stage_create_info.pSpecializationInfo = nullptr;
        shader_stages.push_back(shader_stage_create_info);

        shader_group_create_info.type          = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
        shader_group_create_info.generalShader = static_cast<uint32_t>(shader_stages.size() - 1);
        shader_groups.push_back(shader_group_create_info);
        shader_group_create_info.generalShader = VK_SHADER_UNUSED_KHR;
    }

    Moer::Array<TDescriptorSetLayoutBindingArray> descriptor_bindings;
    Moer::Array<VkPushConstantRange>              push_constant_ranges;
    // find max set index
    int8_t max_set = -1;
    int    sz      = shader_info_list.size();
    for (const auto* meta_shader : shader_info_list) {
        auto layout_infos = meta_shader->GetRootParametersLayoutInfo().GetBindingInfo();
        for (const auto& info : layout_infos) {
            max_set = std::max(max_set, info.space);
        }
    }
    descriptor_bindings.resize(max_set + 1, {});

    // construct layout mappings
    for (const auto* meta_shader : shader_info_list) {
        auto binding_infos  = meta_shader->GetRootParametersLayoutInfo().GetBindingInfo();
        auto constant_infos = meta_shader->GetRootParametersLayoutInfo().GetConstantsInfo();
        
        for (const auto& info : binding_infos) {
            VkDescriptorSetLayoutBinding binding{};
            binding.binding         = info.slot;
            binding.descriptorType  = VulkanEnumTranslator::METoVKDescriptorType(info.type, info.resource_type);
            binding.descriptorCount = info.num;
            binding.stageFlags |= VulkanEnumTranslator::METoVKShaderStageFlags(meta_shader->GetShaderType());
            binding.pImmutableSamplers = nullptr;

            descriptor_bindings[info.space].push_back(std::move(binding));
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

    VulkanRHIRayTracingPipelineState* vk_pso = MoerNew(VulkanRHIRayTracingPipelineState)(m_device);

    vk_pso->InitDescriptorSetLayouts(descriptor_bindings);
    vk_pso->InitPipelineResourceCache(descriptor_bindings);

    const auto& layouts = vk_pso->GetDescriptorSetsLayout()->GetLayouts();
    // create pipeline layout
    VkPipelineLayoutCreateInfo pipeline_layout_create_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipeline_layout_create_info.pNext                  = nullptr;
    pipeline_layout_create_info.flags                  = 0;
    pipeline_layout_create_info.setLayoutCount         = layouts.size();
    pipeline_layout_create_info.pSetLayouts            = layouts.data();
    pipeline_layout_create_info.pushConstantRangeCount = push_constant_ranges.size();
    pipeline_layout_create_info.pPushConstantRanges    = push_constant_ranges.data();

    vk_pso->CreatePipelineLayout(pipeline_layout_create_info);

    VkRayTracingPipelineCreateInfoKHR pipeline_create_info{VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
    pipeline_create_info.stageCount                   = static_cast<uint32_t>(shader_stages.size());
    pipeline_create_info.pStages                      = shader_stages.data();
    pipeline_create_info.groupCount                   = static_cast<uint32_t>(shader_groups.size());
    pipeline_create_info.pGroups                      = shader_groups.data();
    pipeline_create_info.layout                       = vk_pso->GetPipelineLayout();
    pipeline_create_info.maxPipelineRayRecursionDepth = _init.max_ray_recursion_depth;

    vk_pso->CreateRayTracingPipeline(pipeline_create_info);

    //create SBTs
#define ALIGNUP(x, y) ((x + (y - 1)) & (~(y - 1)))

    uint32_t miss_count     = _init.ray_miss_table.size();
    uint32_t hit_count      = _init.ray_hit_table.size();
    uint32_t callable_count = _init.ray_callable_table.size();
    uint32_t handlecount    = 1 + miss_count + hit_count + callable_count;

    auto     rt_props             = VkUtil::QueryPhysicalDeviceExtensionProps<VkPhysicalDeviceRayTracingPipelinePropertiesKHR,
                                                              VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR>(m_device->GetGpu());
    uint32_t handlesize           = rt_props.shaderGroupHandleSize;
    uint32_t handlesize_aligned   = ALIGNUP(handlesize, rt_props.shaderGroupHandleAlignment);
    vk_pso->m_raygen_sbt.size     = ALIGNUP(handlesize_aligned, rt_props.shaderGroupBaseAlignment);
    vk_pso->m_raygen_sbt.stride   = vk_pso->m_raygen_sbt.size;
    vk_pso->m_miss_sbt.size       = ALIGNUP(handlesize_aligned * miss_count, rt_props.shaderGroupBaseAlignment);
    vk_pso->m_miss_sbt.stride     = handlesize_aligned;
    vk_pso->m_hit_sbt.size        = ALIGNUP(handlesize_aligned * hit_count, rt_props.shaderGroupBaseAlignment);
    vk_pso->m_hit_sbt.stride      = handlesize_aligned;
    vk_pso->m_callable_sbt.size   = ALIGNUP(handlesize_aligned * callable_count, rt_props.shaderGroupBaseAlignment);
    vk_pso->m_callable_sbt.stride = handlesize_aligned;

    uint32_t             datasize = handlecount * handlesize;
    Moer::Array<uint8_t> data(datasize);
    VK_CHECK_RESULT(vkGetRayTracingShaderGroupHandlesKHR(m_device->GetDevice(), vk_pso->m_pipeline, 0, handlecount, datasize, data.data()));

    RHIBufferCreateInfo sbt_buffer_ci{};
    sbt_buffer_ci.size   = vk_pso->m_raygen_sbt.size + vk_pso->m_miss_sbt.size + vk_pso->m_hit_sbt.size + vk_pso->m_callable_sbt.size;
    sbt_buffer_ci.usage  = EBufferUsageFlags::SHADER_BINDING_TABLE | EBufferUsageFlags::CPU_VISIBLE;
    sbt_buffer_ci.stride = sbt_buffer_ci.size;
    vk_pso->m_sbt_buffer = RHICreateBufferInner(sbt_buffer_ci);

    VkDeviceAddress sbt_buffer_device_address = GetDeviceAddress(vk_pso->m_sbt_buffer);
    vk_pso->m_raygen_sbt.deviceAddress        = sbt_buffer_device_address;
    vk_pso->m_miss_sbt.deviceAddress          = sbt_buffer_device_address + vk_pso->m_raygen_sbt.size;
    vk_pso->m_hit_sbt.deviceAddress           = sbt_buffer_device_address + vk_pso->m_raygen_sbt.size + vk_pso->m_miss_sbt.size;
    vk_pso->m_callable_sbt.deviceAddress      = sbt_buffer_device_address + vk_pso->m_raygen_sbt.size + vk_pso->m_miss_sbt.size + vk_pso->m_hit_sbt.size;

    uint8_t* pSBTbuffer = static_cast<uint8_t*>(RHIMapBuffer(vk_pso->m_sbt_buffer, 0, sbt_buffer_ci.size));

    uint8_t* pData      = pSBTbuffer;
    auto     getHandle  = [&data, handlesize](int i) { return data.data() + handlesize * i; };
    uint32_t handle_idx = 0;
    memcpy(pData, getHandle(handle_idx++), handlesize);
    pData = pSBTbuffer + vk_pso->m_raygen_sbt.size;

    for (uint32_t i = 0; i < miss_count; ++i) {
        memcpy(pData, getHandle(handle_idx++), handlesize);
        pData += vk_pso->m_miss_sbt.stride;
    }
    pData = pSBTbuffer + vk_pso->m_raygen_sbt.size + vk_pso->m_miss_sbt.size;
    for (uint32_t i = 0; i < hit_count; ++i) {
        memcpy(pData, getHandle(handle_idx++), handlesize);
        pData += vk_pso->m_hit_sbt.stride;
    }
    pData = pSBTbuffer + vk_pso->m_raygen_sbt.size + vk_pso->m_miss_sbt.size + vk_pso->m_hit_sbt.size;
    for (uint32_t i = 0; i < callable_count; ++i) {
        memcpy(pData, getHandle(handle_idx++), handlesize);
        pData += vk_pso->m_callable_sbt.stride;
    }
    RHIUnmapBuffer(vk_pso->m_sbt_buffer);

    return RHIRayTracingPipelineStateRef(vk_pso);
#undef ALIGNUP
}

void VulkanRHIImpl::RHIBatchedBuildRayTracingBLAS(int batch_size, const RHIRayTracingBLASInitializer* _inits, RHIRayTracingBLASRef* results) {

    if (batch_size == 0) {
        LOG_WARNING("RHIBatchedBuildRayTracingBLAS: batch_size == 0");
    }
    if (_inits == nullptr) {
        LOG_WARNING("RHIBatchedBuildRayTracingBLAS: _inits == nullptr");
    }
    if (results == nullptr) {
        LOG_WARNING("RHIBatchedBuildRayTracingBLAS: results == nullptr");
    }
    if (!results || !batch_size || !_inits) {
        return;
    }
    static auto vkGetAccelerationStructureBuildSizesKHR       = reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(vkGetDeviceProcAddr(m_device->GetDevice(), "vkGetAccelerationStructureBuildSizesKHR"));
    static auto vkCreateAccelerationStructureKHR              = reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(vkGetDeviceProcAddr(m_device->GetDevice(), "vkCreateAccelerationStructureKHR"));
    static auto vkCmdBuildAccelerationStructuresKHR           = reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(vkGetDeviceProcAddr(m_device->GetDevice(), "vkCmdBuildAccelerationStructuresKHR"));
    static auto vkDestroyAccelerationStructureKHR             = reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(vkGetDeviceProcAddr(m_device->GetDevice(), "vkDestroyAccelerationStructureKHR"));
    static auto vkCmdCopyAccelerationStructureKHR             = reinterpret_cast<PFN_vkCmdCopyAccelerationStructureKHR>(vkGetDeviceProcAddr(m_device->GetDevice(), "vkCmdCopyAccelerationStructureKHR"));
    static auto vkCmdWriteAccelerationStructuresPropertiesKHR = reinterpret_cast<PFN_vkCmdWriteAccelerationStructuresPropertiesKHR>(vkGetDeviceProcAddr(m_device->GetDevice(), "vkCmdWriteAccelerationStructuresPropertiesKHR"));
    VK_CHECK_NULLPTR(vkGetAccelerationStructureBuildSizesKHR, "RHIBuildRayTracingBLAS: vkGetAccelerationStructureBuildSizesKHR is nullptr", return);
    VK_CHECK_NULLPTR(vkCreateAccelerationStructureKHR, "RHIBuildRayTracingBLAS: vkCreateAccelerationStructureKHR is nullptr", return);
    VK_CHECK_NULLPTR(vkCmdBuildAccelerationStructuresKHR, "RHIBuildRayTracingBLAS: vkCmdBuildAccelerationStructuresKHR is nullptr", return);
    VK_CHECK_NULLPTR(vkDestroyAccelerationStructureKHR, "RHIBuildRayTracingBLAS: vkDestroyAccelerationStructureKHR is nullptr", return);
    VK_CHECK_NULLPTR(vkCmdCopyAccelerationStructureKHR, "RHIBuildRayTracingBLAS: vkCmdCopyAccelerationStructureKHR is nullptr", return);
    VK_CHECK_NULLPTR(vkCmdWriteAccelerationStructuresPropertiesKHR, "RHIBuildRayTracingBLAS: vkCmdWriteAccelerationStructuresPropertiesKHR is nullptr", return);

    int blas_count = batch_size;
    //building caches
    Moer::Array<VkAccelerationStructureBuildGeometryInfoKHR>           all_vk_geo_infos;
    Moer::Array<Moer::Array<VkAccelerationStructureGeometryKHR>>       all_vk_geos;
    Moer::Array<Moer::Array<VkAccelerationStructureBuildRangeInfoKHR>> all_vk_range_infos;
    Moer::Array<const VkAccelerationStructureBuildRangeInfoKHR*>       all_p_vk_range_infos;
    Moer::Array<Moer::Array<uint32_t>>                                 all_primitive_counts;
    Moer::Array<VkAccelerationStructureBuildSizesInfoKHR>              all_vk_size_infos;
    all_vk_geo_infos.reserve(blas_count);
    all_vk_geos.reserve(blas_count);
    all_vk_range_infos.reserve(blas_count);
    all_p_vk_range_infos.reserve(blas_count);
    all_vk_size_infos.reserve(blas_count);

    for (int idx = 0; idx < blas_count; ++idx) {
        const auto& _init = _inits[idx];
        //rhi geometries to vulkan geometries
        Moer::Array<VkAccelerationStructureGeometryKHR> vk_geos;
        vk_geos.reserve(_init.geometries.size());
        for (const auto& rhi_geo : _init.geometries) {
            VkAccelerationStructureGeometryKHR vk_geo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
            vk_geo.geometryType = VulkanRHIRayTracingAccelerationStructure::METoVKGeometryTypeKHR(rhi_geo.geo_type);
            vk_geo.flags        = VulkanRHIRayTracingAccelerationStructure::METoGeometryFlagsKHR(rhi_geo.flags);
            if (vk_geo.geometryType == VK_GEOMETRY_TYPE_TRIANGLES_KHR) {
                const RHIRayTracingTrianglesGeometry&           rhi_triangles = rhi_geo.geometry.triangles;
                VkAccelerationStructureGeometryTrianglesDataKHR vk_triangles{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
                vk_triangles.indexData.deviceAddress = GetDeviceAddress(rhi_triangles.index_buffer);
                vk_triangles.indexType               = VulkanEnumTranslator::METoVKIndexType(rhi_triangles.index_element_type);
                vk_triangles.maxVertex               = rhi_triangles.max_vertex_count - 1;
                if (rhi_triangles.transform_buffer) {
                    vk_triangles.transformData.deviceAddress = GetDeviceAddress(rhi_triangles.transform_buffer);
                }
                vk_triangles.vertexData.deviceAddress = GetDeviceAddress(rhi_triangles.vertex_buffer);
                vk_triangles.vertexFormat             = VulkanEnumTranslator::METoVKFormat(rhi_triangles.vertex_element_type);
                vk_triangles.vertexStride             = rhi_triangles.vertex_buffer_stride;
                vk_geo.geometry.triangles             = vk_triangles;

            } else if (vk_geo.geometryType == VK_GEOMETRY_TYPE_AABBS_KHR) {
                //TODO:impl geometryType: AABB
                auto                                        rhi_aabbs = rhi_geo.geometry.aabbs;
                VkAccelerationStructureGeometryAabbsDataKHR vk_aabbs{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR};
                vk_geo.geometry.aabbs = vk_aabbs;
            }
            vk_geos.emplace_back(vk_geo);
        }
        all_vk_geos.emplace_back(vk_geos);

        //rhi range infos to vulkan range infos
        Moer::Array<VkAccelerationStructureBuildRangeInfoKHR> vk_range_infos;
        Moer::Array<uint32_t>                                 primtive_count;
        vk_range_infos.reserve(_init.range_infos.size());
        primtive_count.reserve(_init.range_infos.size());
        for (const auto& rhi_range_info : _init.range_infos) {
            VkAccelerationStructureBuildRangeInfoKHR vk_range_info;
            vk_range_info.firstVertex     = rhi_range_info.first_vertex;
            vk_range_info.primitiveCount  = rhi_range_info.primitive_count;
            vk_range_info.primitiveOffset = rhi_range_info.primtive_offset;
            vk_range_info.transformOffset = rhi_range_info.transform_offset;
            vk_range_infos.emplace_back(vk_range_info);
            primtive_count.emplace_back(vk_range_info.primitiveCount);
        }
        all_vk_range_infos.emplace_back(vk_range_infos);
        all_primitive_counts.emplace_back(primtive_count);
    }

    int             count_allow_compaction = 0;
    VkDeviceAddress max_scratch_size       = 0;
    for (int idx = 0; idx < blas_count; ++idx) {
        const auto& _init = _inits[idx];
        all_p_vk_range_infos.emplace_back(all_vk_range_infos[idx].data());

        VkAccelerationStructureBuildGeometryInfoKHR vk_geometry_info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        vk_geometry_info.type  = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        vk_geometry_info.flags = VulkanRHIRayTracingAccelerationStructure::METoVKBuildAccelerationStructureFlagsKHR(_init.build_flags);
        if (vk_geometry_info.flags | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR) {
            ++count_allow_compaction;
        }
        vk_geometry_info.mode          = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        vk_geometry_info.ppGeometries  = nullptr;
        vk_geometry_info.geometryCount = static_cast<uint32_t>(all_vk_geos[idx].size());
        vk_geometry_info.pGeometries   = all_vk_geos[idx].data();

        VkAccelerationStructureBuildSizesInfoKHR vk_size_info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        vkGetAccelerationStructureBuildSizesKHR(m_device->GetDevice(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_HOST_OR_DEVICE_KHR, &vk_geometry_info, all_primitive_counts[idx].data(), &vk_size_info);
        all_vk_size_infos.emplace_back(vk_size_info);
        max_scratch_size = std::max(max_scratch_size, vk_size_info.buildScratchSize);

        auto* rhi_blas                                      = new VulkanRHIRayTracingBLAS(_init);
        rhi_blas->size_info.build_scratch_size              = vk_size_info.buildScratchSize;
        rhi_blas->size_info.result_size                     = vk_size_info.accelerationStructureSize;
        RHIBufferCreateInfo blas_buffer_ci                  = RHIBufferCreateInfo::Create(vk_size_info.accelerationStructureSize, vk_size_info.accelerationStructureSize, EBufferUsageFlags::ACCELERATION_STRUCTURE);
        rhi_blas->m_buffer                                  = RHICreateBufferInner(blas_buffer_ci);
        VkBuffer                             vk_blas_buffer = static_cast<VulkanRHIBuffer*>(rhi_blas->m_buffer.Get())->m_alloc.buffer;
        VkAccelerationStructureCreateInfoKHR vk_as_ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
        vk_as_ci.type   = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        vk_as_ci.buffer = vk_blas_buffer;
        vk_as_ci.offset = 0;
        vk_as_ci.size   = vk_size_info.accelerationStructureSize;
        VK_CHECK_RESULT(vkCreateAccelerationStructureKHR(m_device->GetDevice(), &vk_as_ci, nullptr, &rhi_blas->m_blas));

        results[idx] = RHIRayTracingBLASRef(rhi_blas);

        vk_geometry_info.dstAccelerationStructure = rhi_blas->m_blas;
        all_vk_geo_infos.emplace_back(vk_geometry_info);
    }

    RHIBufferCreateInfo scratch_buffer_info           = RHIBufferCreateInfo::Create(max_scratch_size, max_scratch_size, EBufferUsageFlags::ACCELERATION_STRUCTURE_SCRATCH);
    RHIBufferRef        scratch_buffer                = RHICreateBufferInner(scratch_buffer_info);
    VkDeviceAddress     scratch_buffer_device_address = GetDeviceAddress(scratch_buffer);

    const auto& vk_graphic_command_pool = m_device->GetCurrentCommandAllocator()->GetHandle(ECommandListType::GRAPHICS);
    const auto& vk_graphic_queue        = m_device->GetGraphicsQueue();

    //build acceleration structure in batch,but limit the batch size
    Moer::Array<int> indices;
    indices.reserve(blas_count);
    VkDeviceSize batchSize{0};
    VkDeviceSize batchLimit{256'000'000};// 256 MB

    VkQueryPool query_pool = VK_NULL_HANDLE;
    if (count_allow_compaction == blas_count) {
        VkQueryPoolCreateInfo qp_info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qp_info.queryCount = blas_count;
        qp_info.queryType  = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR;
        vkCreateQueryPool(m_device->GetDevice(), &qp_info, nullptr, &query_pool);
    }
    auto cb = BeginSingleTimeCommands(vk_graphic_command_pool);
    for (int idx = 0; idx < blas_count; ++idx) {
        all_vk_geo_infos[idx].scratchData.deviceAddress = scratch_buffer_device_address;
        vkCmdBuildAccelerationStructuresKHR(cb, 1, &all_vk_geo_infos[idx], &all_p_vk_range_infos[idx]);

        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        vkCmdPipelineBarrier(cb,
                             VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                             VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                             0,
                             1,
                             &barrier,
                             0,
                             nullptr,
                             0,
                             nullptr);
        batchSize += all_vk_size_infos[idx].accelerationStructureSize;
        //we need compaction
        if (query_pool) {
            vkCmdPipelineBarrier(cb,
                                 VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                                 VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                 0,
                                 1,
                                 &barrier,
                                 0,
                                 nullptr,
                                 0,
                                 nullptr);
            vkCmdWriteAccelerationStructuresPropertiesKHR(cb,
                                                          1,
                                                          &(static_cast<VulkanRHIRayTracingBLAS*>(results[idx].Get())->m_blas),
                                                          VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR,
                                                          query_pool,
                                                          indices.size());
            indices.emplace_back(idx);
        }
        if (batchSize >= batchLimit || idx == blas_count - 1) {
            if (query_pool) {
                vkResetQueryPool(m_device->GetDevice(), query_pool, 0, indices.size());
            }
            EndSingleTimeCommands(cb, vk_graphic_command_pool, vk_graphic_queue);
            batchSize = 0;
            //we need compaction
            if (query_pool) {
                Moer::Array<VkDeviceSize> compacted_sizes(indices.size());
                vkGetQueryPoolResults(m_device->GetDevice(),
                                      query_pool,
                                      0,
                                      indices.size(),
                                      sizeof(VkDeviceSize) * compacted_sizes.size(),
                                      compacted_sizes.data(),
                                      sizeof(VkDeviceSize),
                                      VK_QUERY_RESULT_WAIT_BIT);
                cb = BeginSingleTimeCommands(vk_graphic_command_pool);
                Moer::Array<VkAccelerationStructureKHR> vk_compacted_ases(indices.size(), VK_NULL_HANDLE);
                Moer::Array<RHIBufferRef>               compacted_buffers(indices.size(), nullptr);
                for (int i = 0; i < indices.size(); ++i) {
                    RHIBufferCreateInfo bf_ci = RHIBufferCreateInfo::Create(compacted_sizes[i], compacted_sizes[i], EBufferUsageFlags::ACCELERATION_STRUCTURE);
                    compacted_buffers[i]      = RHICreateBufferInner(bf_ci);
                    VkAccelerationStructureCreateInfoKHR as_ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
                    as_ci.type   = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
                    as_ci.buffer = static_cast<VulkanRHIBuffer*>(compacted_buffers[i].Get())->GetHandle();
                    as_ci.offset = 0;
                    as_ci.size   = compacted_sizes[i];
                    VK_CHECK_RESULT(vkCreateAccelerationStructureKHR(m_device->GetDevice(), &as_ci, nullptr, &vk_compacted_ases[i]));

                    VkCopyAccelerationStructureInfoKHR as_copy_info{VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR};
                    as_copy_info.src  = static_cast<VulkanRHIRayTracingBLAS*>(results[indices[i]].Get())->m_blas;
                    as_copy_info.dst  = vk_compacted_ases[i];
                    as_copy_info.mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR;
                    vkCmdCopyAccelerationStructureKHR(cb, &as_copy_info);
                }
                EndSingleTimeCommands(cb, vk_graphic_command_pool, vk_graphic_queue);
                for (int i = 0; i < indices.size(); ++i) {
                    VulkanRHIRayTracingBLAS* rhi_blas = static_cast<VulkanRHIRayTracingBLAS*>(results[indices[i]].Get());
                    vkDestroyAccelerationStructureKHR(m_device->GetDevice(), rhi_blas->m_blas, nullptr);
                    rhi_blas->size_info.result_size = compacted_sizes[i];
                    rhi_blas->m_blas                = vk_compacted_ases[i];
                    rhi_blas->m_buffer              = compacted_buffers[i];
                }
                indices.clear();
            }
            if (idx != blas_count - 1) {
                cb = BeginSingleTimeCommands(vk_graphic_command_pool);
            }
        }
    }
    vkDestroyQueryPool(m_device->GetDevice(), query_pool, nullptr);

    return;
}

RHIRayTracingTLASRef VulkanRHIImpl::RHIBuildRayTracingTLAS(const RHIRayTracingTLASInitializer& _init) {
    static auto vkGetAccelerationStructureBuildSizesKHR    = reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(vkGetDeviceProcAddr(m_device->GetDevice(), "vkGetAccelerationStructureBuildSizesKHR"));
    static auto vkCreateAccelerationStructureKHR           = reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(vkGetDeviceProcAddr(m_device->GetDevice(), "vkCreateAccelerationStructureKHR"));
    static auto vkCmdBuildAccelerationStructuresKHR        = reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(vkGetDeviceProcAddr(m_device->GetDevice(), "vkCmdBuildAccelerationStructuresKHR"));
    static auto vkGetAccelerationStructureDeviceAddressKHR = reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(vkGetDeviceProcAddr(m_device->GetDevice(), "vkGetAccelerationStructureDeviceAddressKHR"));
    VK_CHECK_NULLPTR(vkGetAccelerationStructureBuildSizesKHR, "RHIBuildRayTracingTLAS: vkGetAccelerationStructureBuildSizesKHR is nullptr", return RHIRayTracingTLASRef{});
    VK_CHECK_NULLPTR(vkCreateAccelerationStructureKHR, "RHIBuildRayTracingTLAS: vkCreateAccelerationStructureKHR is nullptr", return RHIRayTracingTLASRef{});
    VK_CHECK_NULLPTR(vkCmdBuildAccelerationStructuresKHR, "RHIBuildRayTracingTLAS: vkCmdBuildAccelerationStructuresKHR is nullptr", return RHIRayTracingTLASRef{});
    VK_CHECK_NULLPTR(vkGetAccelerationStructureDeviceAddressKHR, "RHIBuildRayTracingTLAS: vkGetAccelerationStructureDeviceAddressKHR is nullptr", return RHIRayTracingTLASRef{});
    uint32_t                                        instance_count = _init.instances.size();
    Moer::Array<VkAccelerationStructureInstanceKHR> vk_instances;
    vk_instances.reserve(instance_count);
    for (const auto& rhi_instance : _init.instances) {
        VkAccelerationStructureInstanceKHR vk_instance{};
        vk_instance.transform                              = *reinterpret_cast<const VkTransformMatrixKHR*>(&rhi_instance.transform);
        vk_instance.instanceCustomIndex                    = rhi_instance.custom_index;
        vk_instance.mask                                   = rhi_instance.instance_mask;
        vk_instance.flags                                  = VulkanRHIRayTracingAccelerationStructure::METoVKGeometryInstanceFlagsKHR(rhi_instance.flags);
        vk_instance.instanceShaderBindingTableRecordOffset = rhi_instance.instance_sbt_offset;
        VkAccelerationStructureDeviceAddressInfoKHR vk_asda_info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
        vk_asda_info.accelerationStructure         = static_cast<VulkanRHIRayTracingBLAS*>(rhi_instance.blas.Get())->m_blas;
        vk_instance.accelerationStructureReference = vkGetAccelerationStructureDeviceAddressKHR(m_device->GetDevice(), &vk_asda_info);
        vk_instances.emplace_back(vk_instance);
    }
    RHIBufferCreateInfo instance_buffer_info{};
    instance_buffer_info.size                                       = vk_instances.size() * sizeof(VkAccelerationStructureInstanceKHR);
    instance_buffer_info.stride                                     = sizeof(VkAccelerationStructureInstanceKHR);
    instance_buffer_info.usage                                      = EBufferUsageFlags::ACCELERATION_STRUCTURE_BUILD_INPUT;
    RHIBufferRef                                    instance_buffer = CreateBufferFromData(instance_buffer_info, vk_instances.size() * sizeof(VkAccelerationStructureInstanceKHR), vk_instances.data());
    VkAccelerationStructureGeometryInstancesDataKHR vk_insances_data{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
    vk_insances_data.data.deviceAddress = GetDeviceAddress(instance_buffer);

    VkAccelerationStructureGeometryKHR vk_geo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    vk_geo.geometryType       = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    vk_geo.geometry.instances = vk_insances_data;

    VkAccelerationStructureBuildGeometryInfoKHR vk_geo_info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    vk_geo_info.type          = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    vk_geo_info.flags         = VulkanRHIRayTracingAccelerationStructure::METoVKBuildAccelerationStructureFlagsKHR(_init.build_flags);
    vk_geo_info.geometryCount = 1;
    vk_geo_info.mode          = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    vk_geo_info.pGeometries   = &vk_geo;

    VkAccelerationStructureBuildSizesInfoKHR vk_sizes_info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    vkGetAccelerationStructureBuildSizesKHR(m_device->GetDevice(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_HOST_OR_DEVICE_KHR, &vk_geo_info, &instance_count, &vk_sizes_info);

    auto* rhi_tlas                                      = new VulkanRHIRayTracingTLAS(_init);
    rhi_tlas->size_info.build_scratch_size              = vk_sizes_info.buildScratchSize;
    rhi_tlas->size_info.result_size                     = vk_sizes_info.accelerationStructureSize;
    RHIBufferCreateInfo tlas_buffer_ci                  = RHIBufferCreateInfo::Create(vk_sizes_info.accelerationStructureSize, vk_sizes_info.accelerationStructureSize, EBufferUsageFlags::ACCELERATION_STRUCTURE);
    rhi_tlas->m_buffer                                  = RHICreateBufferInner(tlas_buffer_ci);
    VkBuffer                             vk_tlas_buffer = static_cast<VulkanRHIBuffer*>(rhi_tlas->m_buffer.Get())->GetHandle();
    VkAccelerationStructureCreateInfoKHR vk_as_ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    vk_as_ci.type   = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    vk_as_ci.buffer = vk_tlas_buffer;
    vk_as_ci.offset = 0;
    vk_as_ci.size   = vk_sizes_info.accelerationStructureSize;
    VK_CHECK_RESULT(vkCreateAccelerationStructureKHR(m_device->GetDevice(), &vk_as_ci, nullptr, &rhi_tlas->m_tlas));

    RHIBufferCreateInfo scratch_buffer_info           = RHIBufferCreateInfo::Create(vk_sizes_info.buildScratchSize, vk_sizes_info.buildScratchSize, EBufferUsageFlags::ACCELERATION_STRUCTURE_SCRATCH);
    RHIBufferRef        scratch_buffer                = RHICreateBufferInner(scratch_buffer_info);
    VkDeviceAddress     scratch_buffer_device_address = GetDeviceAddress(scratch_buffer);

    vk_geo_info.dstAccelerationStructure  = rhi_tlas->m_tlas;
    vk_geo_info.scratchData.deviceAddress = scratch_buffer_device_address;

    VkAccelerationStructureBuildRangeInfoKHR vk_range_info{};
    vk_range_info.primitiveCount                                    = instance_count;
    const VkAccelerationStructureBuildRangeInfoKHR* p_vk_range_info = &vk_range_info;

    const auto& graphics_command_pool = m_device->GetCurrentCommandAllocator()->GetHandle(ECommandListType::GRAPHICS);
    const auto& graphics_queue        = m_device->GetGraphicsQueue();
    auto        cb                    = BeginSingleTimeCommands(graphics_command_pool);

    vkCmdBuildAccelerationStructuresKHR(cb, 1, &vk_geo_info, &p_vk_range_info);

    EndSingleTimeCommands(cb, graphics_command_pool, graphics_queue);

    return RHIRayTracingTLASRef(rhi_tlas);
}

RHIBufferRef VulkanRHIImpl::RHICreateBufferInner(const RHIBufferCreateInfo& info) {
    RHIBufferInfo buffer_info{};
    buffer_info.size   = info.size;
    buffer_info.usage  = info.usage;
    buffer_info.stride = info.stride;
    if (info.stride == 0) {
        LOG_CRITICAL("RHICreateBufferInner: stride is 0! Set stride to sizeof(std::byte)");
        buffer_info.stride = sizeof(std::byte);
    }
    VulkanRHIBuffer* vk_buffer = MoerNew(VulkanRHIBuffer)(buffer_info);

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

    return (std::byte*)p_data + _offset;
}
void VulkanRHIImpl::RHIUnmapBuffer(RHIBuffer* _buffer) {
    auto* vk_buffer = static_cast<VulkanRHIBuffer*>(_buffer);
    VK_CHECK_NULLPTR(vk_buffer, "RHIUnmapBuffer: buffer to be unmapped is nullptr!", return);

    VmaAllocator allocator = m_device->GetVmaAllocator();
    vmaUnmapMemory(allocator, vk_buffer->m_alloc.alloc);
    // vmaFlushAllocation(allocator, vk_buffer->m_alloc.alloc, 0, VK_WHOLE_SIZE);
}

RHIBufferRef VulkanRHIImpl::RHICreateStagingBuffer(uint64_t _size) {
    RHIBufferCreateInfo info{};
    info.size  = _size;
    info.usage = EBufferUsageFlags::TRANSFER_SRC | EBufferUsageFlags::TRANSFER_DST;

    auto* staging_buffer = m_device->AquireStagingBuffer(info.size);

    return RHIBufferRef(staging_buffer);
}

RHITextureRef VulkanRHIImpl::RHICreateTexture(const RHITextureCreateInfo& info) {
    VulkanRHITexture* vk_texture = MoerNew(VulkanRHITexture)(info, m_device);

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

    image_create_info.initialLayout = VulkanEnumTranslator::METoVKImageLayout(info.layout) == VK_IMAGE_LAYOUT_PREINITIALIZED ? VK_IMAGE_LAYOUT_PREINITIALIZED : VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo alloc_create_info{};
    alloc_create_info.flags = 0;
    alloc_create_info.usage = VulkanMemoryManager::MEGenerateVmaMemoryUsage();

    VmaAllocator allocator = m_device->GetVmaAllocator();
    auto         res       = (vmaCreateImage(allocator, &image_create_info, &alloc_create_info, &vk_texture->m_alloc.image, &vk_texture->m_alloc.alloc, nullptr));
    // VmaAllocationInfo temp_info;
    // vmaGetAllocationInfo(allocator, vk_texture->GetAllocation(), &temp_info);
    return RHITextureRef(vk_texture);
};
bool IsTextureBuffer(EBufferUsageFlags _usage) {
    return uint32_t(_usage & EBufferUsageFlags::TEXTURE_BUFFER) != 0;
}
RHISRVRef VulkanRHIImpl::RHICreateSRVInner(RHIViewableResource* _resource, const RHIViewInfo& _view_info) {

    auto create_texture_srv = [this, _resource, &_view_info]() {
        VulkanRHITextureSRV* vk_srv = MoerNew(VulkanRHITextureSRV)(m_device, _resource, _view_info);

        VkImageViewCreateInfo image_view_create_info{};
        image_view_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        image_view_create_info.pNext = nullptr;
        image_view_create_info.flags = 0;

        auto* vk_texture = static_cast<VulkanRHITexture*>(_resource);
        VK_CHECK_NULLPTR(vk_texture, "RHICreateSRVInner: resource to be viewed is nullptr!", return RHISRVRef{});

        image_view_create_info.image      = vk_texture->GetHandle();
        image_view_create_info.viewType   = VulkanEnumTranslator::METoVKImageViewType(_view_info.texture.srv.dimension);
        image_view_create_info.format     = _view_info.texture.srv.format == PF_UNDEFINED ? VulkanEnumTranslator::METoVKFormat(vk_texture->GetUAVFormat()) : VulkanEnumTranslator::METoVKFormat(_view_info.texture.srv.format);
        image_view_create_info.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
        VkImageAspectFlags flags;
        if (uint32_t(vk_texture->GetUsageFlags() & ETextureUsageFlags::COLOR_ATTACHMENT) != 0) {
            flags = VK_IMAGE_ASPECT_COLOR_BIT;
        } else if (uint32_t(vk_texture->GetUsageFlags() & ETextureUsageFlags::DEPTH_STENCIL_ATTACHMENT) != 0) {
            flags = VK_IMAGE_ASPECT_DEPTH_BIT;
        } else {
            flags = VK_IMAGE_ASPECT_COLOR_BIT;
        }
        image_view_create_info.subresourceRange.aspectMask     = flags;// MARK...
        image_view_create_info.subresourceRange.baseMipLevel   = _view_info.texture.srv.mip_min;
        image_view_create_info.subresourceRange.levelCount     = _view_info.texture.srv.mip_num;
        image_view_create_info.subresourceRange.baseArrayLayer = _view_info.texture.srv.array_min;
        image_view_create_info.subresourceRange.layerCount     = _view_info.texture.srv.array_num;

        // image_view_create_info.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};

        VK_CHECK_RESULT(vkCreateImageView(m_device->GetDevice(), &image_view_create_info, nullptr, &vk_srv->m_view));

        return RHISRVRef(vk_srv);
    };

    auto create_buffer_srv = [this, _resource, &_view_info]() {
        auto*               vk_buffer = static_cast<VulkanRHIBuffer*>(_resource);
        VulkanRHIBufferSRV* vk_srv    = MoerNew(VulkanRHIBufferSRV)(m_device, _resource, _view_info);

        bool b_create_view = IsTextureBuffer(vk_buffer->GetUsage());
        if (!b_create_view) return RHISRVRef(vk_srv);

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
    auto create_acceleration_structure_srv = [this, _resource, &_view_info]() {
        VulkanRHIAccelerationStructureSRV* vk_srv = MoerNew(VulkanRHIAccelerationStructureSRV)(m_device, _resource, _view_info);
        return RHISRVRef(vk_srv);
    };
    if (_view_info.IsBuffer()) {
        return create_buffer_srv();
    }
    if (_view_info.IsTexture()) {
        return create_texture_srv();
    }
    return create_acceleration_structure_srv();
}

RHIUAVRef VulkanRHIImpl::RHICreateUAVInner(RHIViewableResource* _resource, const RHIViewInfo& _view_info) {

    auto create_buffer_uav = [this, _resource, &_view_info]() {
        auto*               vk_buffer = static_cast<VulkanRHIBuffer*>(_resource);
        VulkanRHIBufferUAV* vk_uav    = MoerNew(VulkanRHIBufferUAV)(m_device, _resource, _view_info);
        if (!IsTextureBuffer(vk_buffer->GetUsage())) return RHIUAVRef(vk_uav);
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

    auto create_texture_uav = [this, _resource, &_view_info]() {
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

        image_view_create_info.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
        VkImageAspectFlags flags;
        if (uint32_t(vk_texture->GetUsageFlags() & ETextureUsageFlags::COLOR_ATTACHMENT) != 0) {
            flags = VK_IMAGE_ASPECT_COLOR_BIT;
        } else if (uint32_t(vk_texture->GetUsageFlags() & ETextureUsageFlags::DEPTH_STENCIL_ATTACHMENT) != 0) {
            flags = VK_IMAGE_ASPECT_DEPTH_BIT;
        } else {
            flags = VK_IMAGE_ASPECT_COLOR_BIT;
        }
        image_view_create_info.subresourceRange.aspectMask     = flags;
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
    if (_view_info.IsTexture()) {
        return create_texture_uav();
    }
    LOG_CRITICAL("Acceleration Structure UAV is not implemented yet");
    return nullptr;
}

RHICBVRef VulkanRHIImpl::RHICreateCBV(RHIBuffer* _buffer, uint64_t _byte_size, uint64_t _offset) {
    auto* vk_buffer = static_cast<VulkanRHIBuffer*>(_buffer);
    VK_CHECK_NULLPTR(vk_buffer, "RHICreateCBV: buffer to be viewed is nullptr!", return RHICBVRef{});
    auto create_info = RHIViewInfo::CreateBufferCBVInfo()
                           .SetByteOffset(_offset)
                           .SetStride(_buffer->GetStride())
                           .SetNumElements(_byte_size / _buffer->GetStride());

    VulkanRHICBV* vk_cbv = MoerNew(VulkanRHICBV)(m_device, _buffer, std::move(create_info));
    return RHICBVRef(vk_cbv);
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

RHIComputeCommandList* VulkanRHIImpl::RHICreateComputeCommandList(RHICommandAllocator* _allocator, RHIComputePipelineState* _initial_state) {
    auto* vk_allocator = static_cast<VulkanCommandAllocator*>(_allocator);
    VK_CHECK_NULLPTR(vk_allocator, "RHICreateComputeCommandList: allocator is nullptr!", return nullptr);

    return MoerNew(VulkanRHIComputeCommandList(m_device, vk_allocator->GetHandle(ECommandListType::COMPUTE), VK_COMMAND_BUFFER_LEVEL_PRIMARY));
}

RHIRayTracingCommandList* VulkanRHIImpl::RHICreateRayTracingCommandList(RHICommandAllocator* _allocator, RHIRayTracingPipelineState* _initial_state) {
    auto* vk_allocator = static_cast<VulkanCommandAllocator*>(_allocator);
    VK_CHECK_NULLPTR(vk_allocator, "RHICreateRayTracingCommandList: allocator is nullptr!", return nullptr);

    return MoerNew(VulkanRHIRayTracingCommandList(m_device, vk_allocator->GetHandle(ECommandListType::RAY_TRACING), VK_COMMAND_BUFFER_LEVEL_PRIMARY));
}

// RHIComputeCommandList* VulkanRHIImpl::CreateComputeCommandList(RHIComputePipelineState* _initial_state) {
//     return nullptr;
// }
RHICopyCommandList* VulkanRHIImpl::RHICreateCopyCommandList(RHICommandAllocator* _allocator) {
    auto* vk_allocator = static_cast<VulkanCommandAllocator*>(_allocator);
    VK_CHECK_NULLPTR(vk_allocator, "RHICreateCopyCommandList: allocator is nullptr!", return nullptr);

    return MoerNew(VulkanRHICopyCommandList(m_device, vk_allocator->GetHandle(ECommandListType::COPY), VK_COMMAND_BUFFER_LEVEL_PRIMARY));
}

void VulkanRHIImpl::RHISetBatchedShaderParametersInner(RHIResource* _pso, const RHIBatchedShaderParameters& _batched_params, bool _b_update_constant) {
    const VulkanPipelineState* vk_pso;
    switch (_pso->GetResourceType()) {
        case RRT_GRAPHIC_PIPELINE_STATE:
            vk_pso = static_cast<VulkanRHIGraphicsPipelineState*>(_pso);
            VK_CHECK_NULLPTR(vk_pso, "SetBatchedShaderParameter: graphics pipeline state is nullptr!", return);
            break;
        case RRT_COMPUTE_PIPELINE_STATE:
            vk_pso = static_cast<VulkanRHIComputePipelineState*>(_pso);
            VK_CHECK_NULLPTR(vk_pso, "SetBatchedShaderParameter: compute pipeline state is nullptr!", return);
            break;
        case RRT_RAY_TRACING_PIPELINE_STATE:
            vk_pso = static_cast<VulkanRHIRayTracingPipelineState*>(_pso);
            break;
        default:
            LOG_ERROR("RHISetBatchedShaderParameter: pso is not a pipeline state resource");
            return;
    }
    // resources
    auto* resource_cache = vk_pso->GetPipelineResourceCache();

    for (const auto& params : _batched_params.GetResourceParameters()) {
        auto  type     = params.resource->GetResourceType();
        auto* resource = params.resource.Get();
        if (type == ERHIResourceType::RRT_SAMPLER) {
            // sampler
            auto* vk_sampler = static_cast<VulkanRHISampler*>(resource);
            VK_CHECK_NULLPTR(vk_sampler, "SetBatchedShaderParameter: sampler is nullptr!", break);
            resource_cache->SetSamplerState(params.space, params.slot, vk_sampler);
        } else {
            // view
            auto* view = static_cast<RHIView*>(resource);
            VK_CHECK_NULLPTR(view, "SetBatchedShaderParameter: resource view is nullptr!", break);
            if (view->IsCBV()) {
                auto* vk_buffer = static_cast<RHICBV*>(resource);
                resource_cache->SetCBV(params.space, params.slot, vk_buffer);
            } else if (view->IsSRV()) {
                auto* vk_srv = static_cast<RHISRV*>(resource);
                resource_cache->SetSRV(params.space, params.slot, vk_srv);
            } else if (view->IsUAV()) {
                auto* vk_uav = static_cast<RHIUAV*>(resource);
                resource_cache->SetUAV(params.space, params.slot, vk_uav);
            } else {
                LOG_ERROR("RHISetBatchedShaderParameter: resource view is not a CBV, SRV or UAV!");
                return;
            }
        }
    }

    // cache push constants
    const auto& push_constants = _batched_params.GetConstantParameters();
    // if (!b_update_constant) return;
    for (const auto& params : push_constants) {
        vk_pso->GetPipelineResourceCache()->AddConstantToPush({VulkanEnumTranslator::METoVKShaderStageFlags(params.shader_type),
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
    initializer.enabled_extensions = VulkanDeviceExtension::GetMESupportedDeviceExtensions(m_rhi_info);

    m_device = MoerNew(VulkanDevice)();
    m_device->Init(initializer);
    m_device->InitMemoryAllocator(m_instance);
    RHIViewportInitializer viewport_init{};
    viewport_init.window_handle = Moer::WindowContext::GetMainWindow();
    auto viewport               = RHICreateViewport(viewport_init);
    m_main_viewport             = (VulkanViewport*)viewport.Get();

    m_main_viewport->AddRef();
    // VulkanSwapChain* swap_chain = MoerNew(VulkanSwapChain();
    // swap_chain->Connect(m_instance, m_surface, m_device);
    // uint32_t width, height;

    // swap_chain->Init(&width, &height, Moer::ConfigManager::GetInstance().GetInitConfig().editor_vsync);
    // m_main_viewport = MoerNew(VulkanViewport)(swap_chain, max_frame_in_flight);

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

    return validation_layer_present;
    //return false;
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

RHIBufferRef VulkanRHIImpl::CreateBufferFromData(const RHIBufferCreateInfo& info, uint32_t size, void* data) {
    RHIBufferCreateInfo staging_buffer_info = info;
    staging_buffer_info.usage               = EBufferUsageFlags::TRANSFER_SRC | EBufferUsageFlags::CPU_VISIBLE;
    RHIBufferRef staging_buffer             = RHICreateBufferInner(staging_buffer_info);
    void*        mapped_ptr                 = RHIMapBuffer(staging_buffer, 0, size);
    memcpy(mapped_ptr, data, size);
    RHIUnmapBuffer(staging_buffer);

    RHIBufferCreateInfo buffer_info = info;
    buffer_info.usage |= EBufferUsageFlags::TRANSFER_DST;
    RHIBufferRef buffer = RHICreateBufferInner(buffer_info);

    const auto&  copy_command_pool = m_device->GetCurrentCommandAllocator()->GetHandle(ECommandListType::COPY);
    const auto&  transfer_queue    = m_device->GetTransferQueue();
    auto         cb                = BeginSingleTimeCommands(copy_command_pool);
    VkBuffer     vk_buffer         = static_cast<VulkanRHIBuffer*>(buffer.Get())->GetHandle();
    VkBuffer     vk_staging_buffer = static_cast<VulkanRHIBuffer*>(staging_buffer.Get())->GetHandle();
    VkBufferCopy vk_region{};
    vk_region.size = info.size;
    vkCmdCopyBuffer(cb, vk_staging_buffer, vk_buffer, 1, &vk_region);
    EndSingleTimeCommands(cb, copy_command_pool, transfer_queue);
    return buffer;
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

VkDeviceAddress VulkanRHIImpl::GetDeviceAddress(RHIBufferRef _buffer) {
    auto* vk_buffer = static_cast<VulkanRHIBuffer*>(_buffer.Get());
    VK_CHECK_NULLPTR(vk_buffer, "GetDeviceAddress:buffer to getaddress is nullptr", return VkDeviceAddress{0});
    VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    info.buffer = vk_buffer->GetHandle();
    return vkGetBufferDeviceAddress(m_device->GetDevice(), &info);
}

#pragma endregion

#pragma region viewport
RHIViewport*   VulkanRHIImpl::RHIGetMainViewport() {
    return static_cast<RHIViewport*>(m_main_viewport);
}
//create external viewport
RHIViewportRef VulkanRHIImpl::RHICreateViewport(const RHIViewportInitializer& _init) {
    VulkanSwapChain* swapchain = MoerNew(VulkanSwapChain)();
    uint32_t         width, height;
    VkSurfaceKHR     surface;
    Moer::WindowContext::CreateVulkanSurface(m_instance, _init.window_handle, nullptr, &surface);
    swapchain->Connect(m_instance, surface, m_device);
    swapchain->Init(&width, &height, _init.b_vsync);

    VulkanViewport* viewport = MoerNew(VulkanViewport)(swapchain, m_rhi_info.max_frame_in_flight);

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
