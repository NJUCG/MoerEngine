//
// Created by 74535 on 2023/10/17.
//

#include "rhi/vulkan/misc/VulkanMacroUtils.h"
#include "rhi/vulkan/VulkanCommandList.h"

#include "VulkanDevice.h"
#include "VulkanRHIResource.h"

VulkanRHIGraphicsCommandList::VulkanRHIGraphicsCommandList(VulkanDevice* _device, VkCommandPool _pool, VkCommandBufferLevel _level) : m_device(_device) {
    VkCommandBufferAllocateInfo buffer_alloc_info{};
    buffer_alloc_info.sType              = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_alloc_info.pNext              = nullptr;
    buffer_alloc_info.commandPool        = _pool;
    buffer_alloc_info.level              = _level;
    buffer_alloc_info.commandBufferCount = 1;

    VK_CHECK_RESULT(vkAllocateCommandBuffers(*m_device, &buffer_alloc_info, &m_command_buffer));
}

VulkanRHIGraphicsCommandList::~VulkanRHIGraphicsCommandList() {
    m_device = nullptr;
}

void VulkanRHIGraphicsCommandList::SetBatchedShaderParameter(RHIShaderRef shader, const RHIBatchedShaderParameters& parameters) {
}

void VulkanRHIGraphicsCommandList::SetPipelineState(RHIGraphicsPipelineState* _graphics_pso, const RHIShaderBoundStateInput& _shader_input) {
}

void VulkanRHIGraphicsCommandList::Open() {
    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType            = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.pNext            = nullptr;
    begin_info.flags            = 0;
    begin_info.pInheritanceInfo = nullptr;

    VK_CHECK_RESULT(vkBeginCommandBuffer(m_command_buffer, &begin_info));
}

void VulkanRHIGraphicsCommandList::Close() {
    VK_CHECK_RESULT(vkEndCommandBuffer(m_command_buffer));
}

void VulkanRHIGraphicsCommandList::Reset(RHIGraphicsPipelineState* _graphics_pso) {
}

void VulkanRHIGraphicsCommandList::ClearState(RHIGraphicsPipelineState* _graphics_pso) {
}

void VulkanRHIGraphicsCommandList::DrawIndexedInstanced(uint32_t _index_count, uint32_t _instance_count, int32_t _base_vertex_location, uint32_t _start_instance_location) {
}

void VulkanRHIGraphicsCommandList::DrawIndexedIndirect(RHIBuffer* _argument_buffer, uint64_t _arg_offset, RHIBuffer* _count_buffer, uint64_t _count_buffer_offset, uint32_t _max_draw_count, uint32_t _stride) {
}

void VulkanRHIGraphicsCommandList::Dispatch(uint32_t _group_count_x, uint32_t _group_count_y, uint32_t _group_count_z) {
}

void VulkanRHIGraphicsCommandList::DispatchIndirect(RHIBuffer* _buffer, uint64_t _offset) {
}

void VulkanRHIGraphicsCommandList::CopyBuffer(const RHICopyBufferInfo& _copy_info, RHIBuffer* _src, RHIBuffer* _dst) {
}

void VulkanRHIGraphicsCommandList::CopyTexture(const RHICopyTextureInfo& _copy_info, RHITexture* _src, RHITexture* _dst) {
}

void VulkanRHIGraphicsCommandList::CopyBufferToTexture(RHIBuffer* src_buffer, RHITexture* dst_texture, const RHICopyBufferToTextureInfo& _info) {
}

void VulkanRHIGraphicsCommandList::CopyTextureToBuffer(RHITexture* src_texture, RHIBuffer* dst_buffer, const RHICopyTextureToBufferInfo& _info) {
}

void VulkanRHIGraphicsCommandList::BlitTexture(RHITexture* _src_texture, ETextureLayout _src_layout, RHITexture* _dst_texture, ETextureLayout _dst_layout, RHISubresourceSlice _src_slice, Offset3D* _src_offsets, RHISubresourceSlice _dst_slice, Offset3D* _dst_offsets, ESamplerFilter _filter) {
}

void VulkanRHIGraphicsCommandList::ResolveTexture(RHITexture* _src_texture, ETextureLayout _src_layout, RHITexture* _dst_texture, ETextureLayout _dst_layout, RHISubresourceSlice _src_slice, Offset3D _src_offsets, RHISubresourceSlice _dst_slice, Offset3D _dst_offsets, Extent3D _extent) {
}

void VulkanRHIGraphicsCommandList::SetPipelineBarrier(const RHIBarrierDependencyInfo& _dependency) {
}

void VulkanRHIGraphicsCommandList::SetCullMode(ERasterizerCullMode _cull_mode) {
}

void VulkanRHIGraphicsCommandList::SetPrimitiveTopology(EPrimitiveTopology _topology) {
}

void VulkanRHIGraphicsCommandList::SetViewPorts(uint32_t num_viewports, const ViewPort* p_viewports) {
}

void VulkanRHIGraphicsCommandList::SetViewPort(const ViewPort& _viewport) {
}

void VulkanRHIGraphicsCommandList::SetScissors(uint32_t num_scissors, const Rect2D* p_scissors) {
}

void VulkanRHIGraphicsCommandList::SetScissor(const Rect2D& _scissor) {
}

void VulkanRHIGraphicsCommandList::SetBlendFactors(const float* _factors) {
}

void VulkanRHIGraphicsCommandList::BindVertexBuffers(uint32_t _start_index, uint32_t _num_buffers, const RHIBuffer* p_vertex_buffers) {
}

void VulkanRHIGraphicsCommandList::BindIndexBuffer(const RHIBuffer* p_index_buffer) {
}

void VulkanRHIGraphicsCommandList::ClearDepthStencil() {
}

void VulkanRHIGraphicsCommandList::ClearUAVInt(RHIUnorderedAccessView* _uav, const Moer::Vector4i& _values) {
}

void VulkanRHIGraphicsCommandList::ClearUAVFloat(RHIUnorderedAccessView* _uav, const Moer::Vector4f& _values) {
}

void VulkanRHIGraphicsCommandList::BeginRenderPass(const RHIRenderPassInfo& _pass_info, const char* _pass_name) {
    VkRenderingInfo dynamic_rendering_info{};
    dynamic_rendering_info.sType                    = VK_STRUCTURE_TYPE_RENDERING_INFO;
    dynamic_rendering_info.pNext                    = nullptr;
    dynamic_rendering_info.flags                    = 0;
    dynamic_rendering_info.renderArea.offset.x      = _pass_info.render_area.offset.x;
    dynamic_rendering_info.renderArea.offset.y      = _pass_info.render_area.offset.y;
    dynamic_rendering_info.renderArea.extent.width  = _pass_info.render_area.extent.width;
    dynamic_rendering_info.renderArea.extent.height = _pass_info.render_area.extent.height;
    dynamic_rendering_info.layerCount               = 1;
    dynamic_rendering_info.viewMask                 = 0;

    const uint32_t num_color_attachments = _pass_info.GetNumColorAttachments();

    std::vector<VkRenderingAttachmentInfo> color_attachments(num_color_attachments);
    for (uint32_t i = 0; i < num_color_attachments; ++i) {
        color_attachments[i] = FromColorAttachmentInfo(_pass_info.color_attachments[i]);
    }
    VkRenderingAttachmentInfo depth_stencil_attachment = FromDepthStencilAttachmentInfo(_pass_info.depth_stencil_attachment);

    dynamic_rendering_info.colorAttachmentCount = num_color_attachments;
    dynamic_rendering_info.pColorAttachments    = color_attachments.data();
    dynamic_rendering_info.pDepthAttachment     = &depth_stencil_attachment;
    dynamic_rendering_info.pStencilAttachment   = &depth_stencil_attachment;

    vkCmdBeginRendering(m_command_buffer, &dynamic_rendering_info);
}

void VulkanRHIGraphicsCommandList::EndRenderPass() {
    vkCmdEndRenderingKHR(m_command_buffer);
}

void VulkanRHIGraphicsCommandList::NextSubpass() {
}

void VulkanRHIGraphicsCommandList::BeginQuery(RHIRenderQuery* _query) {
}

void VulkanRHIGraphicsCommandList::EndQuery(RHIRenderQuery* _query) {
}

void VulkanRHIGraphicsCommandList::GetQueryData(ERenderQueryType _query_type, uint32_t _first_index, uint32_t _num_queries, RHIBuffer* _dst_buffer, uint64_t _dst_offset) {
}

void VulkanRHIGraphicsCommandList::ExecuteSubCommands(uint32_t _num, RHIGraphicsCommandList* _sub_commands) {
}

#pragma region ray-tracing

void VulkanRHIGraphicsCommandList::BuildAccelerationStructure(RHIBuffer* _instance_data, uint64_t _instance_offset, bool _b_update, RHIBuffer* _scratch, RHIBuffer* _scratch_offset) {
}

#pragma endregion

VkRenderingAttachmentInfo VulkanRHIGraphicsCommandList::FromColorAttachmentInfo(const RHIRenderPassInfo::ColorAttachmentInfo& _color_attachment_info) const {
    VkRenderingAttachmentInfo attachment_info{};
    attachment_info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    attachment_info.pNext = nullptr;

    auto* texture_view = _color_attachment_info.color_attachment_view.texture_view;

    if (texture_view->IsSRV()) {
        auto* texture_srv         = static_cast<VulkanRHIShaderResourceView*>(texture_view);
        attachment_info.imageView = texture_srv->GetView();
    } else if (texture_view->IsUAV()) {
        auto* texture_uav         = static_cast<VulkanRHIUnorderedAccessView*>(texture_view);
        attachment_info.imageView = texture_uav->GetView();
    } else {
        LOG_CRITICAL("Invalid texture view type: {}.", typeid(*texture_view).name());
        return attachment_info;
    }

    attachment_info.imageLayout                 = VulkanEnumTranslator::METoVKImageLayout(_color_attachment_info.color_attachment_view.required_layout);
    attachment_info.loadOp                      = VulkanEnumTranslator::METoVKAttachmentLoadOp(GetLoadOp(_color_attachment_info.color_attachment_action));
    attachment_info.storeOp                     = VulkanEnumTranslator::METoVKAttachmentStoreOp(GetStoreOp(_color_attachment_info.color_attachment_action));
    attachment_info.clearValue.color.float32[0] = _color_attachment_info.color_attachment_view.clear_attachment.value.color.float32[0];
    attachment_info.clearValue.color.float32[1] = _color_attachment_info.color_attachment_view.clear_attachment.value.color.float32[1];
    attachment_info.clearValue.color.float32[2] = _color_attachment_info.color_attachment_view.clear_attachment.value.color.float32[2];
    attachment_info.clearValue.color.float32[3] = _color_attachment_info.color_attachment_view.clear_attachment.value.color.float32[3];

    auto* resolve_texture_view = _color_attachment_info.resolve_attachment_view.texture_view;
    if (resolve_texture_view->IsSRV()) {
        auto* resolve_texture_srv        = static_cast<VulkanRHIShaderResourceView*>(resolve_texture_view);
        attachment_info.resolveImageView = resolve_texture_srv->GetView();
    } else if (resolve_texture_view->IsUAV()) {
        auto* resolve_texture_uav        = static_cast<VulkanRHIUnorderedAccessView*>(resolve_texture_view);
        attachment_info.resolveImageView = resolve_texture_uav->GetView();
    } else {
        LOG_CRITICAL("Invalid resolve texture view type: {}.", typeid(*resolve_texture_view).name());
        return attachment_info;
    }

    attachment_info.resolveMode        = VK_RESOLVE_MODE_AVERAGE_BIT;
    attachment_info.resolveImageLayout = VulkanEnumTranslator::METoVKImageLayout(_color_attachment_info.resolve_attachment_view.required_layout);

    return attachment_info;
}

VkRenderingAttachmentInfo VulkanRHIGraphicsCommandList::FromDepthStencilAttachmentInfo(const RHIRenderPassInfo::DepthStencilAttachmentInfo& _depth_stencil_attachment_info) const {
    VkRenderingAttachmentInfo attachment_info{};
    attachment_info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    attachment_info.pNext = nullptr;

    auto* depth_stencil_view = _depth_stencil_attachment_info.depth_stencil_attachment_view.texture_view;

    if (depth_stencil_view->IsSRV()) {
        auto* depth_stencil_srv   = static_cast<VulkanRHIShaderResourceView*>(depth_stencil_view);
        attachment_info.imageView = depth_stencil_srv->GetView();
    } else if (depth_stencil_view->IsUAV()) {
        auto* depth_stencil_uav   = static_cast<VulkanRHIUnorderedAccessView*>(depth_stencil_view);
        attachment_info.imageView = depth_stencil_uav->GetView();
    } else {
        LOG_CRITICAL("Invalid depth view type: {}.", typeid(*depth_stencil_view).name());
        return attachment_info;
    }

    attachment_info.imageLayout                     = VulkanEnumTranslator::METoVKImageLayout(_depth_stencil_attachment_info.depth_stencil_attachment_view.required_layout);
    attachment_info.loadOp                          = VulkanEnumTranslator::METoVKAttachmentLoadOp(GetLoadOp(_depth_stencil_attachment_info.depth_stencil_action));
    attachment_info.storeOp                         = VulkanEnumTranslator::METoVKAttachmentStoreOp(GetStoreOp(_depth_stencil_attachment_info.depth_stencil_action));
    attachment_info.clearValue.depthStencil.depth   = _depth_stencil_attachment_info.depth_stencil_attachment_view.clear_attachment.value.depth_stencil.depth;
    attachment_info.clearValue.depthStencil.stencil = _depth_stencil_attachment_info.depth_stencil_attachment_view.clear_attachment.value.depth_stencil.stencil;

    return attachment_info;
}
