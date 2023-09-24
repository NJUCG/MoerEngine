#ifndef RHI_COMMAND_LIST_H
#define RHI_COMMAND_LIST_H
#include "RHI.h"
class RHICommandListBase {
protected:
    RHI_API RHICommandListBase();

public:
    RHI_API ~RHICommandListBase();

    virtual void BeginRendering() = 0;
    virtual void EndRendering()   = 0;
};

class RHIGraphicsCommandList final : public RHICommandListBase {
public:
    virtual void SetPipelineState(RHIGraphicsPipelineState* _graphics_pso) = 0;
    virtual void Close()                                                   = 0;
    virtual void Reset(RHIGraphicsPipelineState* _graphics_pso)            = 0;
    virtual void ClearState(RHIGraphicsPipelineState* _graphics_pso)       = 0;

    virtual void DrawIndexedInstanced(uint32_t _index_count, uint32_t _instance_count, int32_t _base_vertex_location, uint32_t _start_instance_location) = 0;

    virtual void Dispatch(uint32_t _group_count_x, uint32_t _group_count_y, uint32_t _group_count_z) = 0;

    virtual void DispatchIndirect(RHIBuffer* _buffer, uint64_t _offset) = 0;

    virtual void CopyBuffer(const RHICopyBufferInfo& _copy_info, RHIBuffer* _src, RHIBuffer* _dst)                            = 0;
    virtual void CopyTexture(const RHICopyTextureInfo& _copy_info, RHITexture* _src, RHITexture* _dst)                        = 0;
    virtual void CopyBufferToTexture(RHIBuffer* src_buffer, RHITexture* dst_texture, const RHICopyBufferToTextureInfo& _info) = 0;

    virtual void CopyTextureToBuffer(RHITexture* src_texture, RHIBuffer* dst_buffer, const RHICopyTextureToBufferInfo& _info) = 0;


    //To copy regions of a source texture into a destination texture, potentially performing format conversion, arbitrary scaling, and filtering.
    //must not be used for multi-sampled source or destination textures, use resolve instead
    virtual void BlitTexture(RHITexture*         _src_texture,
                             ETextureLayout      _src_layout,
                             RHITexture*         _dst_texture,
                             ETextureLayout      _dst_layout,
                             RHISubresourceSlice _src_slice,
                             Offset3D            _src_offsets[2],
                             RHISubresourceSlice _dst_slice,
                             Offset3D            _dst_offsets[2],
                             ESamplerFilter      _filter) = 0;

    //To resolve a multi-sample color texture to a non-multisample color texture
    virtual void ResolveTexture(
        RHITexture*    _src_texture,
        ETextureLayout _src_layout,
        RHITexture*    _dst_texture,
        ETextureLayout _dst_layout,

        RHISubresourceSlice _src_slice,
        Offset3D            _src_offsets,
        RHISubresourceSlice _dst_slice,
        Offset3D            _dst_offsets,
        Extent3D            _extent) = 0;

    virtual void SetPipelineBarrier(const RHIBarrierDependencyInfo& _dependency) = 0;

    virtual void SetCullMode(ERasterizerCullMode _cull_mode)                       = 0;
    virtual void SetPrimitiveTopology(EPrimitiveTopology _topology)                = 0;
    virtual void SetViewPorts(uint32_t num_viewports, const ViewPort* p_viewports) = 0;
    virtual void SetViewPort(const ViewPort& _viewport)                            = 0;
    virtual void SetScissors(uint32_t num_scissors, const Rect2D* p_scissors)      = 0;
    virtual void SetScissor(const Rect2D& _scissor)                                = 0;
    virtual void SetBlendFactors(const float _factors[4])                          = 0;
 
    virtual void BindVertexBuffers(
        uint32_t   _start_index,
        uint32_t   _num_buffers,
        RHIBuffer* p_vertex_buffers) = 0;

    virtual void BeginRendering() = 0;
    virtual void EndRendering()   = 0;
};

class RHIComputeCommandList final : public RHICommandListBase {
};
#endif//RHI_COMMAND_LIST_H
