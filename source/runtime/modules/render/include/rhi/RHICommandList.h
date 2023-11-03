#ifndef RHI_COMMAND_LIST_H
#define RHI_COMMAND_LIST_H
#include "RHI.h"
#include "math/Base.h"
#include "rhi/RHIResource.h"
class Shader;
class RHICommandListBase {
protected:
    RHI_API RHICommandListBase();

public:
    virtual void SetBatchedShaderParameter(RHIShaderRef shader, const RHIBatchedShaderParameters& parameters) = 0;
    RHI_API virtual ~RHICommandListBase();

    virtual void* GetNativeHandle() const { return nullptr; }
};

class RHIGraphicsCommandList : public RHICommandListBase {
public:
    virtual ~RHIGraphicsCommandList(){};
    virtual void SetBatchedShaderParameter(RHIShaderRef shader, const RHIBatchedShaderParameters& parameters) = 0;
    virtual void SetPipelineState(RHIGraphicsPipelineState* _graphics_pso)                                    = 0;
    virtual void Open() {}
    virtual void Close()                                                  = 0;
    virtual void Reset(RHIGraphicsPipelineState* _graphics_pso = nullptr) = 0;
    virtual void ClearState(RHIGraphicsPipelineState* _graphics_pso)      = 0;

    virtual void DrawIndexedInstanced(uint32_t _index_count, uint32_t _instance_count, uint32_t _start_index_location, int32_t _base_vertex_location) = 0;

    virtual void DrawIndexedIndirect(
        RHIBuffer* _argument_buffer,
        uint64_t   _arg_offset,
        RHIBuffer* _count_buffer,
        uint64_t   _count_buffer_offset,
        uint32_t   _max_draw_count,
        uint32_t   _stride) = 0;

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
        uint32_t            _start_index,
        uint32_t            _num_buffers,
        const RHIBufferRef* p_vertex_buffers,
        const uint32_t*     _offsets) = 0;

    virtual void BindIndexBuffer(
        const RHIBuffer*  p_index_buffer,
        uint32_t          _offset,
        EIndexElementType _type) = 0;

    virtual void SetAttachments() {
    }

    virtual void ClearDepthStencil() = 0;
    virtual void ClearUAVInt(
        RHIUnorderedAccessView* _uav,
        const Moer::Vector4i&   _values) = 0;
    virtual void ClearUAVFloat(
        RHIUnorderedAccessView* _uav,
        const Moer::Vector4f&   _values) = 0;

    virtual void BeginRenderPass(const RHIRenderPassInfo& _pass_info, const char* _pass_name) = 0;
    virtual void EndRenderPass()                                                              = 0;

    virtual void NextSubpass() = 0;

    //todo: query data declaration
    virtual void BeginQuery(RHIRenderQuery* _query) = 0;
    virtual void EndQuery(RHIRenderQuery* _query)   = 0;

    virtual void GetQueryData(
        ERenderQueryType _query_type,
        uint32_t         _first_index,
        uint32_t         _num_queries,
        RHIBuffer*       _dst_buffer,
        uint64_t         _dst_offset) = 0;

    virtual void ExecuteSubCommands(uint32_t                _num,
                                    RHIGraphicsCommandList* _sub_commands) = 0;

    virtual void BindParameters(Shader* shader, RHIBatchedShaderParameters* batched_params){};
#pragma region ray-tracing
    virtual void BuildAccelerationStructure(
        RHIBuffer* _instance_data,
        uint64_t   _instance_offset,
        bool       _b_update,
        RHIBuffer* _scratch,
        RHIBuffer* _scratch_offset) = 0;

#pragma endregion
};

class RHIComputeCommandList : public RHICommandListBase {
    virtual void SetBatchedShaderParameter(RHIShaderRef shader, const RHIBatchedShaderParameters& parameters) = 0;
};
#endif//RHI_COMMAND_LIST_H
