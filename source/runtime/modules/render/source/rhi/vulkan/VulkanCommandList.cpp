//
// Created by 74535 on 2023/10/17.
//

#include "rhi/vulkan/misc/VulkanMacroUtils.h"
#include "rhi/vulkan/VulkanCommandList.h"

#include "VulkanDevice.h"

VulkanGraphicsCommandList::VulkanGraphicsCommandList(VulkanDevice* _device, VkCommandPool _pool, VkCommandBufferLevel _level) : m_device(_device) {
    VkCommandBufferAllocateInfo buffer_alloc_info{};
    buffer_alloc_info.sType              = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_alloc_info.pNext              = nullptr;
    buffer_alloc_info.commandPool        = _pool;
    buffer_alloc_info.level              = _level;
    buffer_alloc_info.commandBufferCount = 1;

    VK_CHECK_RESULT(vkAllocateCommandBuffers(*m_device, &buffer_alloc_info, &m_command_buffer));
}

VulkanGraphicsCommandList::~VulkanGraphicsCommandList() {
    m_device = nullptr;
}

void VulkanGraphicsCommandList::SetPipelineState(RHIGraphicsPipelineState* _graphics_pso, const RHIShaderBoundStateInput& _shader_input) {
}

void VulkanGraphicsCommandList::Open() {
    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType            = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.pNext            = nullptr;
    begin_info.flags            = 0;
    begin_info.pInheritanceInfo = nullptr;

    VK_CHECK_RESULT(vkBeginCommandBuffer(m_command_buffer, &begin_info));
}

void VulkanGraphicsCommandList::Close() {
    VK_CHECK_RESULT(vkEndCommandBuffer(m_command_buffer));
}

void VulkanGraphicsCommandList::Reset(RHIGraphicsPipelineState* _graphics_pso) {
}

void VulkanGraphicsCommandList::ClearState(RHIGraphicsPipelineState* _graphics_pso) {
}

void VulkanGraphicsCommandList::DrawIndexedInstanced(uint32_t _index_count, uint32_t _instance_count, int32_t _base_vertex_location, uint32_t _start_instance_location) {
}

void VulkanGraphicsCommandList::DrawIndexedIndirect(RHIBuffer* _argument_buffer, uint64_t _arg_offset, RHIBuffer* _count_buffer, uint64_t _count_buffer_offset, uint32_t _max_draw_count, uint32_t _stride) {
}

void VulkanGraphicsCommandList::Dispatch(uint32_t _group_count_x, uint32_t _group_count_y, uint32_t _group_count_z) {
}

void VulkanGraphicsCommandList::DispatchIndirect(RHIBuffer* _buffer, uint64_t _offset) {
}

void VulkanGraphicsCommandList::CopyBuffer(const RHICopyBufferInfo& _copy_info, RHIBuffer* _src, RHIBuffer* _dst) {
}

void VulkanGraphicsCommandList::CopyTexture(const RHICopyTextureInfo& _copy_info, RHITexture* _src, RHITexture* _dst) {
}

void VulkanGraphicsCommandList::CopyBufferToTexture(RHIBuffer* src_buffer, RHITexture* dst_texture, const RHICopyBufferToTextureInfo& _info) {
}

void VulkanGraphicsCommandList::CopyTextureToBuffer(RHITexture* src_texture, RHIBuffer* dst_buffer, const RHICopyTextureToBufferInfo& _info) {
}

void VulkanGraphicsCommandList::BlitTexture(RHITexture* _src_texture, ETextureLayout _src_layout, RHITexture* _dst_texture, ETextureLayout _dst_layout, RHISubresourceSlice _src_slice, Offset3D* _src_offsets, RHISubresourceSlice _dst_slice, Offset3D* _dst_offsets, ESamplerFilter _filter) {
}

void VulkanGraphicsCommandList::ResolveTexture(RHITexture* _src_texture, ETextureLayout _src_layout, RHITexture* _dst_texture, ETextureLayout _dst_layout, RHISubresourceSlice _src_slice, Offset3D _src_offsets, RHISubresourceSlice _dst_slice, Offset3D _dst_offsets, Extent3D _extent) {
}

void VulkanGraphicsCommandList::SetPipelineBarrier(const RHIBarrierDependencyInfo& _dependency) {
}

void VulkanGraphicsCommandList::SetCullMode(ERasterizerCullMode _cull_mode) {
}

void VulkanGraphicsCommandList::SetPrimitiveTopology(EPrimitiveTopology _topology) {
}

void VulkanGraphicsCommandList::SetViewPorts(uint32_t num_viewports, const ViewPort* p_viewports) {
}

void VulkanGraphicsCommandList::SetViewPort(const ViewPort& _viewport) {
}

void VulkanGraphicsCommandList::SetScissors(uint32_t num_scissors, const Rect2D* p_scissors) {
}

void VulkanGraphicsCommandList::SetScissor(const Rect2D& _scissor) {
}

void VulkanGraphicsCommandList::SetBlendFactors(const float* _factors) {
}

void VulkanGraphicsCommandList::BindVertexBuffers(uint32_t _start_index, uint32_t _num_buffers, const RHIBuffer* p_vertex_buffers) {
}

void VulkanGraphicsCommandList::BindIndexBuffer(const RHIBuffer* p_index_buffer) {
}

void VulkanGraphicsCommandList::ClearDepthStencil() {
}

void VulkanGraphicsCommandList::ClearUAVInt(RHIUnorderedAccessView* _uav, const Moer::Vector4i& _values) {
}

void VulkanGraphicsCommandList::ClearUAVFloat(RHIUnorderedAccessView* _uav, const Moer::Vector4f& _values) {
}

void VulkanGraphicsCommandList::BeginRenderPass(const RHIRenderPassInfo& _pass_info, const char* _pass_name) {
}

void VulkanGraphicsCommandList::EndRenderPass() {
}

void VulkanGraphicsCommandList::NextSubpass() {
}

void VulkanGraphicsCommandList::BeginQuery(RHIRenderQuery* _query) {
}

void VulkanGraphicsCommandList::EndQuery(RHIRenderQuery* _query) {
}

void VulkanGraphicsCommandList::GetQueryData(ERenderQueryType _query_type, uint32_t _first_index, uint32_t _num_queries, RHIBuffer* _dst_buffer, uint64_t _dst_offset) {
}

void VulkanGraphicsCommandList::ExecuteSubCommands(uint32_t _num, RHIGraphicsCommandList* _sub_commands) {
}

void VulkanGraphicsCommandList::BuildAccelerationStructure(RHIBuffer* _instance_data, uint64_t _instance_offset, bool _b_update, RHIBuffer* _scratch, RHIBuffer* _scratch_offset) {
}
