
#ifndef VULKAN_COMMAND_LIST_H
#define VULKAN_COMMAND_LIST_H
#include "rhi/RHICommandList.h"

#include <vulkan.h>

class VulkanDevice;

class VulkanRHIGraphicsCommandList final : public RHIGraphicsCommandList {
public:
    VulkanRHIGraphicsCommandList(VulkanDevice* _device, VkCommandPool _pool, VkCommandBufferLevel _level = VK_COMMAND_BUFFER_LEVEL_PRIMARY);
    ~VulkanRHIGraphicsCommandList();

    void SetBatchedShaderParameter(RHIShaderRef shader, const RHIBatchedShaderParameters& parameters) override;
    void SetPipelineState(RHIGraphicsPipelineState* _graphics_pso, const RHIShaderBoundStateInput& _shader_input) override;
    void Open() override;
    void Close() override;
    void Reset(RHIGraphicsPipelineState* _graphics_pso) override;
    void ClearState(RHIGraphicsPipelineState* _graphics_pso) override;

    void DrawIndexedInstanced(uint32_t _index_count, uint32_t _instance_count, int32_t _base_vertex_location, uint32_t _start_instance_location) override;

    void DrawIndexedIndirect(
        RHIBuffer* _argument_buffer,
        uint64_t   _arg_offset,
        RHIBuffer* _count_buffer,
        uint64_t   _count_buffer_offset,
        uint32_t   _max_draw_count,
        uint32_t   _stride) override;

    void Dispatch(uint32_t _group_count_x, uint32_t _group_count_y, uint32_t _group_count_z) override;

    void DispatchIndirect(RHIBuffer* _buffer, uint64_t _offset) override;

    void CopyBuffer(const RHICopyBufferInfo& _copy_info, RHIBuffer* _src, RHIBuffer* _dst) override;
    void CopyTexture(const RHICopyTextureInfo& _copy_info, RHITexture* _src, RHITexture* _dst) override;
    void CopyBufferToTexture(RHIBuffer* src_buffer, RHITexture* dst_texture, const RHICopyBufferToTextureInfo& _info) override;

    void CopyTextureToBuffer(RHITexture* src_texture, RHIBuffer* dst_buffer, const RHICopyTextureToBufferInfo& _info) override;

    //To copy regions of a source texture into a destination texture, potentially performing format conversion, arbitrary scaling, and filtering.
    //must not be used for multi-sampled source or destination textures, use resolve instead
    void BlitTexture(RHITexture*         _src_texture,
                     ETextureLayout      _src_layout,
                     RHITexture*         _dst_texture,
                     ETextureLayout      _dst_layout,
                     RHISubresourceSlice _src_slice,
                     Offset3D            _src_offsets[2],
                     RHISubresourceSlice _dst_slice,
                     Offset3D            _dst_offsets[2],
                     ESamplerFilter      _filter) override;

    //To resolve a multi-sample color texture to a non-multisample color texture
    void ResolveTexture(
        RHITexture*    _src_texture,
        ETextureLayout _src_layout,
        RHITexture*    _dst_texture,
        ETextureLayout _dst_layout,

        RHISubresourceSlice _src_slice,
        Offset3D            _src_offsets,
        RHISubresourceSlice _dst_slice,
        Offset3D            _dst_offsets,
        Extent3D            _extent) override;

    void SetPipelineBarrier(const RHIBarrierDependencyInfo& _dependency) override;

    void SetCullMode(ERasterizerCullMode _cull_mode) override;
    void SetPrimitiveTopology(EPrimitiveTopology _topology) override;
    void SetViewPorts(uint32_t num_viewports, const ViewPort* p_viewports) override;
    void SetViewPort(const ViewPort& _viewport) override;
    void SetScissors(uint32_t num_scissors, const Rect2D* p_scissors) override;
    void SetScissor(const Rect2D& _scissor) override;
    void SetBlendFactors(const float _factors[4]) override;

    void BindVertexBuffers(
        uint32_t         _start_index,
        uint32_t         _num_buffers,
        const RHIBuffer* p_vertex_buffers) override;

    void BindIndexBuffer(
        const RHIBuffer* p_index_buffer) override;

    void SetAttachments() override {
    }

    void ClearDepthStencil() override;
    void ClearUAVInt(
        RHIUnorderedAccessView* _uav,
        const Moer::Vector4i&   _values) override;
    void ClearUAVFloat(
        RHIUnorderedAccessView* _uav,
        const Moer::Vector4f&   _values) override;

    void BeginRenderPass(const RHIRenderPassInfo& _pass_info, const char* _pass_name) override;
    void EndRenderPass() override;

    void NextSubpass() override;

    //todo: query data declaration
    void BeginQuery(RHIRenderQuery* _query) override;
    void EndQuery(RHIRenderQuery* _query) override;

    void GetQueryData(
        ERenderQueryType _query_type,
        uint32_t         _first_index,
        uint32_t         _num_queries,
        RHIBuffer*       _dst_buffer,
        uint64_t         _dst_offset) override;

    void ExecuteSubCommands(uint32_t                _num,
                            RHIGraphicsCommandList* _sub_commands) override;

#pragma region ray-tracing
    void BuildAccelerationStructure(
        RHIBuffer* _instance_data,
        uint64_t   _instance_offset,
        bool       _b_update,
        RHIBuffer* _scratch,
        RHIBuffer* _scratch_offset) override;

#pragma endregion

private:
    VulkanDevice*   m_device;
    VkCommandBuffer m_command_buffer;

private:
    VkRenderingAttachmentInfo FromColorAttachmentInfo(const RHIRenderPassInfo::ColorAttachmentInfo& _color_attachment_info) const;
    VkRenderingAttachmentInfo FromDepthStencilAttachmentInfo(const RHIRenderPassInfo::DepthStencilAttachmentInfo& _depth_stencil_attachment_info) const;
};

#endif//VULKAN_COMMAND_LIST_H
