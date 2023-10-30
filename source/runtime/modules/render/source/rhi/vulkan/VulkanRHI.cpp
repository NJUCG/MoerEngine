#include "config.h"

#include "rhi/RHIResource.h"
#include "rhi/vulkan/misc/VulkanMacroUtils.h"
#include "misc/MacroUtils.h"

#include "rhi/vulkan/VulkanRHI.h"
#include "rhi/vulkan/VulkanCommandList.h"

#include "VulkanRHIResource.h"
#include "VulkanRHIInitializer.h"
#include "VulkanExtension.h"

#include "shader/Shader.h"

#include "VulkanDebug.h"
#include "VulkanUtil.h"

#include "VulkanDevice.h"
#include "VulkanSwapChain.h"

#include "shader/Shader.h"
#include "shader/ShaderResource.h"

#include <GLFW/glfw3.h>

#include <unordered_map>
#include <string>
#include <set>

namespace VkUtil = Moer::RHI::Vulkan::Util;

VulkanRHIImpl::VulkanRHIImpl(GLFWwindow* _window) : m_instance(VK_NULL_HANDLE), m_device(nullptr), m_current_viewport(nullptr) {
    LOG_INFO("Built with Vulkan header version {0:d}.{1:d}.{2:d}", VK_API_VERSION_MAJOR(VK_HEADER_VERSION_COMPLETE), VK_API_VERSION_MINOR(VK_HEADER_VERSION_COMPLETE), VK_API_VERSION_PATCH(VK_HEADER_VERSION_COMPLETE));

    CreateInstance();
    InitSurface(_window);
}

void VulkanRHIImpl::Initialize() {
    InitVulkan();
    InitVulkanMemoryAllocator();
}

void VulkanRHIImpl::PostInit() {
    LOG_INFO("VulkanRHIImpl::PostInit()");
}

void VulkanRHIImpl::ShutDown() {
    delete m_swap_chain;
    delete m_device;
}

#pragma region resources creation
RHISamplerRef  VulkanRHIImpl::RHICreateSampler(const RHISamplerInitializer& _initializer) {
    VulkanRHISampler* vk_sampler = new VulkanRHISampler();
    vk_sampler->GenerateSamplerFromInitializer(m_device, _initializer);

    return RHISamplerRef(vk_sampler);
}

RHIRasterizationStateRef VulkanRHIImpl::RHICreateRasterizationState(const RHIRasterizationStateInitializer& _init) {
    VulkanRHIRasterizationState* vk_rasterization_state = new VulkanRHIRasterizationState();
    vk_rasterization_state->GenerateRasterizationStateFromInitializer(_init);

    return RHIRasterizationStateRef(vk_rasterization_state);
}

RHIDepthStencilStateRef VulkanRHIImpl::RHICreateDepthStencilState(const RHIDepthStencilStateInitializer& _init) {
    VulkanRHIDepthStencilState* vk_depth_stencil_state = new VulkanRHIDepthStencilState();
    vk_depth_stencil_state->GenerateDepthStencilStateFromInitializer(_init);

    return RHIDepthStencilStateRef(vk_depth_stencil_state);
}

RHIMultisampleStateRef VulkanRHIImpl::RHICreateMultiSampleState(const RHIMultisampleStateInitializer& _init) {
    VulkanRHIMultisampleState* vk_multisample_state = new VulkanRHIMultisampleState();
    vk_multisample_state->GenerateMultisampleStateFromInitializer(_init);

    return RHIMultisampleStateRef(vk_multisample_state);
}

RHIBlendStateRef VulkanRHIImpl::RHICreateBlendState(const RHIBlendStateInitializer& _init) {
    VulkanRHIBlendState* vk_blend_state = new VulkanRHIBlendState();
    vk_blend_state->GenerateBlendStateFromInitializer(_init);

    return RHIBlendStateRef(vk_blend_state);
}

RHIVertexInputStateRef VulkanRHIImpl::RHICreateVertexInputState(const VertexInputStateInitializerList& _init) {
    VulkanRHIVertexInputState* vk_input_state = new VulkanRHIVertexInputState();
    vk_input_state->GenerateVertexInputStateFromInitializer(_init);

    return RHIVertexInputStateRef(vk_input_state);
}

RHIVertexShaderRef VulkanRHIImpl::RHICreateVertexShader(const Shader* shader) {
    auto* vk_shader = new VulkanRHIVertexShader(shader);
    vk_shader->CreateShaderModule(m_device, shader->GetCodeEntry()->code);

    return RHIVertexShaderRef(vk_shader);
}

RHIFragmentShaderRef VulkanRHIImpl::RHICreateFragmentShader(const Shader* shader) {
    auto* vk_shader = new VulkanRHIFragmentShader(shader);
    vk_shader->CreateShaderModule(m_device, shader->GetCodeEntry()->code);

    return RHIFragmentShaderRef(vk_shader);
}

RHIGeometryShaderRef VulkanRHIImpl::RHICreateGeometryShader(const Shader* shader) {
    auto* vk_shader = new VulkanRHIGeometryShader(shader);
    vk_shader->CreateShaderModule(m_device, shader->GetCodeEntry()->code);

    return RHIGeometryShaderRef(vk_shader);
}

RHIMeshShaderRef VulkanRHIImpl::RHICreateMeshShader(const Shader* shader) {
    auto* vk_shader = new VulkanRHIMeshShader(shader);
    vk_shader->CreateShaderModule(m_device, shader->GetCodeEntry()->code);

    return RHIMeshShaderRef(vk_shader);
}

RHIAmplificationShaderRef VulkanRHIImpl::RHICreateAmplificationShader(const Shader* shader) {
    auto* vk_shader = new VulkanRHIAmplificationShader(shader);
    vk_shader->CreateShaderModule(m_device, shader->GetCodeEntry()->code);

    return RHIAmplificationShaderRef(vk_shader);
}

RHIComputeShaderRef VulkanRHIImpl::RHICreateComputeShader(const Shader* shader) {
    auto* vk_shader = new VulkanRHIComputeShader(shader);
    vk_shader->CreateShaderModule(m_device, shader->GetCodeEntry()->code);

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

RHIGraphicsPipelineStateRef VulkanRHIImpl::RHICreateGraphicsPipelineState(const RHIGraphicsPipelineStateInitializer& _init) {
    VulkanRHIGraphicsPipelineState* vk_pipeline = new VulkanRHIGraphicsPipelineState();

    // rendering create info
    VkPipelineRenderingCreateInfo rendering_create_info{};
    rendering_create_info.sType                = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering_create_info.pNext                = nullptr;
    rendering_create_info.viewMask             = 0;
    rendering_create_info.colorAttachmentCount = _init.color_attachment_count;
    std::vector<VkFormat> color_attachment_formats(_init.color_attachment_count);
    for (int i = 0; i < _init.color_attachment_count; ++i) {
        color_attachment_formats[i] = VkFormat(_init.color_attachment_formats[i]);
    }
    rendering_create_info.pColorAttachmentFormats = color_attachment_formats.data();
    rendering_create_info.depthAttachmentFormat   = VulkanEnumTranslator::METoVKFormat(_init.depth_stencil_format);
    rendering_create_info.stencilAttachmentFormat = VulkanEnumTranslator::METoVKFormat(_init.depth_stencil_format);

    // shader stage
    auto shader_stages = VulkanRHIGraphicsPipelineState::METoVKShaderStageCreateInfo(_init.shader_stage);

    // vertex input state
    auto vertex_input_state = VulkanRHIGraphicsPipelineState::METoVKVertexInputStateCreateInfo(*_init.shader_stage.p_vertex_input_state);

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
    auto* vk_rasterizer_state = static_cast<VulkanRHIRasterizationState*>(_init.rasterizer_state);
    VK_CHECK_NULLPTR(vk_rasterizer_state, "RHICreateGraphicsPipelineState: initializer's rasterization state is nullptr!", RHIGraphicsPipelineStateRef{});
    auto rasterization_state = vk_rasterizer_state->GetHandle();

    // multisample state
    auto* vk_multisample_state = static_cast<VulkanRHIMultisampleState*>(_init.multisample_state);
    VK_CHECK_NULLPTR(vk_multisample_state, "RHICreateGraphicsPipelineState: initializer's multisample state is nullptr!", RHIGraphicsPipelineStateRef{});
    auto multisample_state = vk_multisample_state->GetHandle();

    // depth stencil state
    auto* vk_depth_stencil_state = static_cast<VulkanRHIDepthStencilState*>(_init.depth_stencil_state);
    VK_CHECK_NULLPTR(vk_depth_stencil_state, "RHICreateGraphicsPipelineState: initializer's depth stencil state is nullptr!", RHIGraphicsPipelineStateRef{});
    auto depth_stencil_state = vk_depth_stencil_state->GetHandle();

    // color blend state
    auto* vk_blend_state = static_cast<VulkanRHIBlendState*>(_init.blend_state);
    VK_CHECK_NULLPTR(vk_blend_state, "RHICreateGraphicsPipelineState: initializer's color blend state is nullptr!", RHIGraphicsPipelineStateRef{});
    auto color_blend_state = vk_blend_state->GetHandle();

    // dynamic state
    std::array<VkDynamicState, 2>    states = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic_state{};
    dynamic_state.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state.pNext             = nullptr;
    dynamic_state.flags             = 0;
    dynamic_state.dynamicStateCount = states.size();
    dynamic_state.pDynamicStates    = states.data();

    // pipeline layout
    std::vector<Shader*> shaders;

    using VulkanDescriptorSetLayout = std::pair<VkDescriptorSetLayout, std::vector<VkDescriptorSetLayoutBinding>>;

    std::unordered_map<uint8_t, VulkanDescriptorSetLayout> layout_mappings;

    // construct layout mappings
    for (const auto* shader : shaders) {
        auto infos = shader->GetRootParametersLayoutInfo().GetLayoutInfos();

        for (const auto& info : infos) {
            VkDescriptorSetLayoutBinding binding;
            binding.binding            = info.slot;
            binding.descriptorType     = VulkanEnumTranslator::METoVKDescriptorType(info.type);
            binding.descriptorCount    = 1;
            binding.stageFlags         = VulkanEnumTranslator::METoVKShaderStageFlags(shader->GetShaderType());
            binding.pImmutableSamplers = nullptr;

            layout_mappings[info.space].second.push_back(binding);
        }
    }

    // create descriptor set layouts
    for (auto& [space, layout] : layout_mappings) {
        VkDescriptorSetLayoutCreateInfo layout_create_info{};
        layout_create_info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout_create_info.pNext        = nullptr;
        layout_create_info.flags        = 0;
        layout_create_info.bindingCount = layout.second.size();
        layout_create_info.pBindings    = layout.second.data();

        VK_CHECK_RESULT(vkCreateDescriptorSetLayout(*m_device, &layout_create_info, nullptr, &layout.first));
    }

    // extract descriptor set layouts
    std::vector<VkDescriptorSetLayout> layouts;
    for (auto& [space, layout] : layout_mappings) {
        layouts.push_back(layout.first);
    }

    // create pipeline layout
    VkPipelineLayoutCreateInfo pipeline_layout_create_info{};
    pipeline_layout_create_info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_create_info.pNext                  = nullptr;
    pipeline_layout_create_info.flags                  = 0;
    pipeline_layout_create_info.setLayoutCount         = layouts.size();
    pipeline_layout_create_info.pSetLayouts            = layouts.data();
    pipeline_layout_create_info.pushConstantRangeCount = 0;
    pipeline_layout_create_info.pPushConstantRanges    = nullptr;

    VkPipelineLayout pipeline_layout;
    vkCreatePipelineLayout(*m_device, &pipeline_layout_create_info, nullptr, &pipeline_layout);

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
    pipeline_create_info.layout              = pipeline_layout;
    pipeline_create_info.basePipelineHandle  = nullptr;// MARK...
    pipeline_create_info.basePipelineIndex   = -1;

    VK_CHECK_RESULT(vkCreateGraphicsPipelines(*m_device, nullptr, 1, &pipeline_create_info, nullptr, &vk_pipeline->m_pipeline));

    return RHIGraphicsPipelineStateRef(vk_pipeline);
}

RHIComputePipelineStateRef VulkanRHIImpl::RHICreateComputePipelineState(RHIComputeShader* _compute_shader) { return RHIComputePipelineStateRef{}; }

void VulkanRHIImpl::RHIUploadBuffer(RHIBufferRef _buffer_ref, const uint8_t* _data, uint32_t _size) {
    auto* vk_buffer = static_cast<VulkanRHIBuffer*>(_buffer_ref.Get());
    VK_CHECK_NULLPTR(vk_buffer, "RHIUploadBuffer: buffer to be uploaded is nullptr!");

    VulkanRHIBuffer* staging_buffer = static_cast<VulkanRHIBuffer*>(RHICreateBuffer({_size, 1, EBufferUsageFlags::TRANSFER_SRC}).Get());

    void* p_data;
    VK_CHECK_RESULT(vmaMapMemory(m_allocator, staging_buffer->m_alloc.alloc, &p_data));
    memcpy(p_data, _data, _size);
    vmaUnmapMemory(m_allocator, staging_buffer->m_alloc.alloc);

    CopyBuffer(staging_buffer, vk_buffer);
}

void VulkanRHIImpl::RHICopyBuffer(RHIBuffer* _src, RHIBuffer* _dst) {
    auto* src = static_cast<VulkanRHIBuffer*>(_src);
    auto* dst = static_cast<VulkanRHIBuffer*>(_dst);
    if (src == nullptr || dst == nullptr) {
        LOG_CRITICAL("RHICopyBuffer: buffer src or dst is nullptr, src: {}, dst: {}.", typeid(_src).name(), typeid(_dst).name());
        return;
    }
    if (_src->GetInfo().size != _dst->GetInfo().size) {
        LOG_CRITICAL("RHICopyBuffer: Source buffer size {} is not equal to destination buffer size {}.", _src->GetInfo().size, _dst->GetInfo().size);
        return;
    }

    CopyBuffer(src, dst);
}

RHIBufferRef VulkanRHIImpl::RHICreateBuffer(const RHIBufferCreateInfo& info) {
    RHIBufferInfo buffer_info{};
    buffer_info.size   = info.size;
    buffer_info.stride = info.stride;
    buffer_info.usage  = info.usage;

    VulkanRHIBuffer* vk_buffer = new VulkanRHIBuffer(buffer_info);

    VkBufferCreateInfo buffer_create_info{};
    buffer_create_info.sType                 = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_create_info.pNext                 = nullptr;
    buffer_create_info.flags                 = 0;
    buffer_create_info.size                  = info.size;
    buffer_create_info.usage                 = VulkanRHIBuffer::METoVKBufferUsageFlags(m_device, info.usage);
    buffer_create_info.sharingMode           = VK_SHARING_MODE_EXCLUSIVE;
    buffer_create_info.queueFamilyIndexCount = 0;
    buffer_create_info.pQueueFamilyIndices   = nullptr;

    VmaAllocationCreateInfo alloc_create_info{};
    alloc_create_info.flags = 0;
    alloc_create_info.usage = VulkanMemoryManager::MEGenerateVmaMemoryUsage();

    VK_CHECK_RESULT(vmaCreateBuffer(m_allocator, &buffer_create_info, &alloc_create_info, &vk_buffer->m_alloc.buffer, &vk_buffer->m_alloc.alloc, nullptr));

    return RHIBufferRef(vk_buffer);
}

void* VulkanRHIImpl::RHIMapBuffer(RHIBuffer* _buffer, uint64_t _offset, uint64_t _size) {
    auto* vk_buffer = static_cast<VulkanRHIBuffer*>(_buffer);
    VK_CHECK_NULLPTR(vk_buffer, "RHIMapBuffer: buffer to be mapped is nullptr!", nullptr);

    void* p_data;
    VK_CHECK_RESULT(vmaMapMemory(m_allocator, vk_buffer->m_alloc.alloc, &p_data));

    return p_data;
}
void VulkanRHIImpl::RHIUnmapBuffer(RHIBuffer* _buffer) {
    auto* vk_buffer = static_cast<VulkanRHIBuffer*>(_buffer);
    VK_CHECK_NULLPTR(vk_buffer, "RHIUnmapBuffer: buffer to be unmapped is nullptr!");

    vmaUnmapMemory(m_allocator, vk_buffer->m_alloc.alloc);
}

RHITextureRef VulkanRHIImpl::RHICreateTexture(const RHITextureCreateInfo& info) {
    VulkanRHITexture* vk_texture = new VulkanRHITexture(info);

    VkImageCreateInfo image_create_info{};
    image_create_info.sType                 = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_create_info.pNext                 = nullptr;
    image_create_info.flags                 = 0;
    image_create_info.imageType             = VulkanRHITexture::METoVKImageType(info.dimension);
    image_create_info.format                = VulkanEnumTranslator::METoVKFormat(info.format);
    image_create_info.extent.width          = info.extent.x;
    image_create_info.extent.height         = info.extent.y;
    image_create_info.extent.depth          = info.depth;
    image_create_info.mipLevels             = info.num_mips;
    image_create_info.arrayLayers           = info.array_size;
    image_create_info.samples               = VulkanEnumTranslator::METoVKSampleCountFlagBits(info.num_samples);
    image_create_info.tiling                = VK_IMAGE_TILING_OPTIMAL;// MARK...
    image_create_info.usage                 = VulkanRHITexture::METoVKImageUsageFlags(info.usage);
    image_create_info.sharingMode           = VK_SHARING_MODE_EXCLUSIVE;
    image_create_info.queueFamilyIndexCount = 0;
    image_create_info.pQueueFamilyIndices   = nullptr;
    image_create_info.initialLayout         = VulkanEnumTranslator::METoVKImageLayout(info.layout);

    VmaAllocationCreateInfo alloc_create_info{};
    alloc_create_info.flags = 0;
    alloc_create_info.usage = VulkanMemoryManager::MEGenerateVmaMemoryUsage();

    VK_CHECK_RESULT(vmaCreateImage(m_allocator, &image_create_info, &alloc_create_info, &vk_texture->m_alloc.image, &vk_texture->m_alloc.alloc, nullptr));

    return RHITextureRef(vk_texture);
};

RHIShaderResourceViewRef VulkanRHIImpl::RHICreateShaderResourceView(RHIViewableResource* _resource, const RHIViewInfo& _view_info) {
    VulkanRHIShaderResourceView* vk_srv = new VulkanRHIShaderResourceView(_resource, _view_info);

    VkImageViewCreateInfo image_view_create_info{};
    image_view_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    image_view_create_info.pNext = nullptr;
    image_view_create_info.flags = 0;

    auto* vk_texture = static_cast<VulkanRHITexture*>(_resource);
    VK_CHECK_NULLPTR(vk_texture, "RHICreateShaderResourceView: resource to be viewed is nullptr!", RHIShaderResourceViewRef{});

    image_view_create_info.image                           = vk_texture->GetHandle();
    image_view_create_info.viewType                        = VulkanEnumTranslator::METoVKImageViewType(_view_info.texture.srv.dimension);
    image_view_create_info.format                          = VulkanEnumTranslator::METoVKFormat(_view_info.texture.srv.format);
    image_view_create_info.components                      = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    image_view_create_info.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;// MARK...
    image_view_create_info.subresourceRange.baseMipLevel   = _view_info.texture.srv.mip_min;
    image_view_create_info.subresourceRange.levelCount     = _view_info.texture.srv.mip_num;
    image_view_create_info.subresourceRange.baseArrayLayer = _view_info.texture.srv.array_min;
    image_view_create_info.subresourceRange.layerCount     = _view_info.texture.srv.array_num;

    VK_CHECK_RESULT(vkCreateImageView(*m_device, &image_view_create_info, nullptr, &vk_srv->m_view));

    return RHIShaderResourceViewRef(vk_srv);
}

RHIUnorderedAccessViewRef VulkanRHIImpl::RHICreateUnorderedAccessView(RHIViewableResource* _resource, const RHIViewInfo& _view_info) {
    VulkanRHIUnorderedAccessView* vk_uav = new VulkanRHIUnorderedAccessView(_resource, _view_info);

    VkImageViewCreateInfo image_view_create_info{};
    image_view_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    image_view_create_info.pNext = nullptr;
    image_view_create_info.flags = 0;

    auto* vk_texture = static_cast<VulkanRHITexture*>(_resource);
    VK_CHECK_NULLPTR(vk_texture, "RHICreateUnorderedAccessView: resource to be viewed is nullptr!", RHIUnorderedAccessViewRef{});

    image_view_create_info.image                           = vk_texture->GetHandle();
    image_view_create_info.viewType                        = VulkanEnumTranslator::METoVKImageViewType(_view_info.texture.uav.dimension);
    image_view_create_info.format                          = VulkanEnumTranslator::METoVKFormat(_view_info.texture.uav.format);
    image_view_create_info.components                      = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    image_view_create_info.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;// MARK...
    image_view_create_info.subresourceRange.baseMipLevel   = _view_info.texture.uav.mip_min;
    image_view_create_info.subresourceRange.levelCount     = _view_info.texture.uav.mip_num;
    image_view_create_info.subresourceRange.baseArrayLayer = _view_info.texture.uav.array_min;
    image_view_create_info.subresourceRange.layerCount     = _view_info.texture.uav.array_num;

    VK_CHECK_RESULT(vkCreateImageView(*m_device, &image_view_create_info, nullptr, &vk_uav->m_view));

    return RHIUnorderedAccessViewRef(vk_uav);
}

RHICommandQueue* VulkanRHIImpl::CreateCommandQueue(ECommandQueueType type) {
    return nullptr;
}

RHIGraphicsCommandList* VulkanRHIImpl::CreateGraphicsCommandList(RHIGraphicsPipelineState* _initial_state) {
    return new VulkanRHIGraphicsCommandList(m_device, m_device->GetDefaultCommandPool(), VK_COMMAND_BUFFER_LEVEL_PRIMARY);
}

RHIComputeCommandList* VulkanRHIImpl::CreateComputeCommandList(RHIComputePipelineState* _initial_state) {
    return nullptr;
}

RHIShaderRef VulkanRHIImpl::RHICreateShader(Shader* shader) {
    const ShaderCodeEntry* compiled_code = shader->GetCodeEntry();

    //information needed for pipeline layout creation
    const auto& params_layout = shader->GetRootParametersLayoutInfo();

    for (auto& param_layout : params_layout.GetLayoutInfos()) {
        if (param_layout.IsValid()) {
            //means correctly corresponding param in target shader
        }
    }

    //todo: create shader from information above

    return nullptr;
}

#pragma endregion

void VulkanRHIImpl::InitSurface(GLFWwindow* _window) {
    VK_CHECK_RESULT(glfwCreateWindowSurface(m_instance, _window, nullptr, &m_surface));
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

    m_swap_chain = new VulkanSwapChain();
    m_swap_chain->Connect(m_instance, m_surface, m_device);
    uint32_t width, height;
    // glfwGetFramebufferSize(m_window, &width, &height);
    m_swap_chain->Init(&width, &height, true);
}

void VulkanRHIImpl::InitVulkanMemoryAllocator() {
    VmaAllocatorCreateInfo alloc_create_info{};

    VmaVulkanFunctions vma_functions{};
    vma_functions.vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)vkGetInstanceProcAddr;
    vma_functions.vkGetDeviceProcAddr   = (PFN_vkGetDeviceProcAddr)vkGetDeviceProcAddr;

    alloc_create_info.vulkanApiVersion = VK_API_VERSION_1_3;

    alloc_create_info.instance         = m_instance;
    alloc_create_info.physicalDevice   = m_device->GetGpu();
    alloc_create_info.device           = m_device->GetDevice();
    alloc_create_info.pVulkanFunctions = &vma_functions;

    VK_CHECK_RESULT(vmaCreateAllocator(&alloc_create_info, &m_allocator));

    LOG_INFO("Vulkan Memory Allocator initialized with api version: {}.", alloc_create_info.vulkanApiVersion);
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

    std::vector<const char*> r_extensions(n, nullptr);
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
    std::vector<VkLayerProperties> instance_layer_properties(instance_layer_count);
    vkEnumerateInstanceLayerProperties(&instance_layer_count, instance_layer_properties.data());
    bool validation_layer_present = false;

    for (auto layer_property : instance_layer_properties) {
        if (layer_name == layer_property.layerName) {
            validation_layer_present = true;
            break;
        }
    }

    return validation_layer_present;
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
    VK_CHECK_RESULT(vkAllocateCommandBuffers(*m_device, &alloc_info, &command_buffer));

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

    vkFreeCommandBuffers(*m_device, _pool, 1, &_command_buffer);
}

void VulkanRHIImpl::CopyBuffer(VulkanRHIBuffer* _src, VulkanRHIBuffer* _dst) {
    auto transfer_pool  = m_device->GetTransferCommandPool();
    auto command_buffer = BeginSingleTimeCommands(transfer_pool);

    VkBufferCopy copy_region{};
    copy_region.srcOffset = 0;
    copy_region.dstOffset = 0;
    copy_region.size      = _src->GetInfo().size;
    vkCmdCopyBuffer(command_buffer, _src->GetHandle(), _dst->GetHandle(), 1, &copy_region);

    EndSingleTimeCommands(command_buffer, transfer_pool, m_device->GetTransferQueue());
}

#pragma endregion
