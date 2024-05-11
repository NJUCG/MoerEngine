#ifndef MOER_ENGINE_RHI_COMMAND_H
#define MOER_ENGINE_RHI_COMMAND_H

#include "math/Base.h"
#include "misc/STL.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "RenderAPI.h"

class Shader;

class RHICommandAllocator {
public:
    virtual ~RHICommandAllocator() = default;
    virtual void     Reset()       = 0;
    ECommandListType GetType() const { return list_type; }

private:
    ECommandListType list_type;
};

struct RHIBlitTextureInfo {

    ETextureLayout      src_layout;
    ETextureLayout      dst_layout;
    RHISubresourceSlice src_slice;
    Offset3D            src_offsets[2];
    RHISubresourceSlice dst_slice;
    Offset3D            dst_offsets[2];
    ESamplerFilter      filter;

public:
    RHIBlitTextureInfo() { memset(this, 0, sizeof(RHIBlitTextureInfo)); }
};

struct RHIResolveTextureInfo {
    ETextureLayout      src_layout;
    ETextureLayout      dst_layout;
    RHISubresourceSlice src_slice;
    Offset3D            src_offset;
    RHISubresourceSlice dst_slice;
    Offset3D            dst_offset;
    Extent3D            extent;

public:
    RHIResolveTextureInfo() { memset(this, 0, sizeof(RHIResolveTextureInfo)); }
};

class RHICommandListBase {
protected:
    RENDER_API RHICommandListBase();

public:
    RENDER_API virtual ~RHICommandListBase();

    virtual void* GetNativeHandle() const = 0;
    virtual void  BeginRecording()        = 0;
    virtual void  EndRecording()          = 0;
    virtual void  Reset()                 = 0;
};

class RHIGraphicsCommandList : public RHICommandListBase {
public:
    virtual ~RHIGraphicsCommandList(){};
    virtual void SetPipelineState(RHIGraphicsPipelineState* _graphics_pso) = 0;
    virtual void SetPipelineState(RHIComputePipelineState* _compute_pso)   = 0;
    // virtual void Open()                                                    = 0;
    // virtual void Close()                                                   = 0;
    // virtual void Reset()                                                   = 0;
    virtual void ClearState(RHIGraphicsPipelineState* _graphics_pso) = 0;

    virtual void DrawIndexedInstanced(
        uint32_t _index_count,
        uint32_t _instance_count,
        uint32_t _start_index_location,
        uint32_t _start_vertex_location,
        uint32_t _start_instance_location) = 0;

    virtual void DrawIndexedIndirect(
        RHIBuffer* _argument_buffer,
        uint64_t   _arg_offset,
        RHIBuffer* _count_buffer,
        uint64_t   _count_buffer_offset,
        uint32_t   _max_draw_count,
        uint32_t   _stride) = 0;

    virtual void Draw(
        uint32_t _vertex_count,
        uint32_t _instance_count,
        uint32_t _start_vertex_location,
        uint32_t _start_instance_location) = 0;

    void Dispatch(Moer::Vector3i _group_count) {
        Dispatch(_group_count.x, _group_count.y, _group_count.z);
    }
    virtual void TransitionTexture(RHITexture* _texture, ETextureUsageFlags _usage, EPassType _dst_pass, uint8_t _mip_idx = 0, uint8_t _mip_cnt = 1) = 0;
    virtual void ExecuteTransition()                                                                                                                 = 0;
    virtual void Dispatch(uint32_t _group_count_x, uint32_t _group_count_y, uint32_t _group_count_z)                                                 = 0;

    virtual void DispatchIndirect(RHIBuffer* _buffer, uint64_t _offset) = 0;

    virtual void CopyBuffer(const RHICopyBufferInfo& _copy_info, RHIBuffer* _src, RHIBuffer* _dst)                            = 0;
    virtual void CopyTexture(const RHICopyTextureInfo& _copy_info, RHITexture* _src, RHITexture* _dst)                        = 0;
    virtual void CopyBufferToTexture(const RHICopyBufferToTextureInfo& _info, RHIBuffer* src_buffer, RHITexture* dst_texture) = 0;

    virtual void CopyTextureToBuffer(const RHICopyTextureToBufferInfo& _info, RHITexture* src_texture, RHIBuffer* dst_buffer) = 0;

    //To copy regions of a source texture into a destination texture, potentially performing format conversion, arbitrary scaling, and filtering.
    //must not be used for multi-sampled source or destination textures, use resolve instead
    virtual void BlitTexture(const RHIBlitTextureInfo& _blit_info, RHITexture* _src, RHITexture* _dst) = 0;

    //To resolve a multi-sample color texture to a non-multisample color texture
    virtual void ResolveTexture(const RHIResolveTextureInfo& _resolve_info, RHITexture* _src, RHITexture* _dst) = 0;

    virtual void SetPipelineBarrier(const RHIBarrierDependencyInfo& _dependency) = 0;

    virtual void SetCullMode(ERasterizerCullMode _cull_mode)                       = 0;
    virtual void SetPrimitiveTopology(EPrimitiveTopology _topology)                = 0;
    virtual void SetViewPorts(uint32_t num_viewports, const ViewPort* p_viewports) = 0;
    virtual void SetViewPort(const ViewPort& _viewport)                            = 0;
    virtual void SetScissors(uint32_t num_scissors, const Rect2D* p_scissors)      = 0;
    virtual void SetScissor(const Rect2D& _scissor)                                = 0;
    virtual void SetBlendFactors(const float _factors[4])                          = 0;
    virtual void BeginLabel(const char* _label)                                    = 0;
    virtual void EndLabel()                                                        = 0;

    virtual void BindVertexBuffers(
        uint32_t            _start_index,
        uint32_t            _num_buffers,
        const RHIBufferRef* p_vertex_buffers,
        const uint32_t*     _offsets) = 0;

    virtual void BindIndexBuffer(
        const RHIBuffer*  p_index_buffer,
        uint32_t          _offset,
        EIndexElementType _type) = 0;

    virtual void FillBuffer(RHIBuffer* _buffer, uint64_t _offset, uint64_t _size, uint32_t _data) = 0;

    virtual void SetAttachments() {
    }

    virtual void ClearDepthStencil() = 0;
    virtual void ClearUAVInt(
        RHIUAV*               _uav,
        const Moer::Vector4i& _values) = 0;
    virtual void ClearUAVFloat(
        RHIUAV*               _uav,
        const Moer::Vector4f& _values) = 0;

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

    virtual void BindParameters(Shader* shader, RHIBatchedShaderParameters* batched_params) {};
};

class RHIComputeCommandList : public RHICommandListBase {
public:
    virtual ~RHIComputeCommandList(){};
    virtual void SetPipelineState(RHIComputePipelineState* _compute_pso)                             = 0;
    virtual void Dispatch(uint32_t _group_count_x, uint32_t _group_count_y, uint32_t _group_count_z) = 0;
    virtual void DispatchIndirect(RHIBuffer* _buffer, uint64_t _offset)                              = 0;

    virtual void CopyBuffer(const RHICopyBufferInfo& _copy_info, RHIBuffer* _src, RHIBuffer* _dst)                            = 0;
    virtual void CopyTexture(const RHICopyTextureInfo& _copy_info, RHITexture* _src, RHITexture* _dst)                        = 0;
    virtual void CopyBufferToTexture(const RHICopyBufferToTextureInfo& _info, RHIBuffer* src_buffer, RHITexture* dst_texture) = 0;

    virtual void CopyTextureToBuffer(const RHICopyTextureToBufferInfo& _info, RHITexture* src_texture, RHIBuffer* dst_buffer) = 0;

    virtual void SetPipelineBarrier(const RHIBarrierDependencyInfo& _dependency) = 0;
};

class RHIRayTracingCommandList : public RHICommandListBase {
public:
    virtual ~RHIRayTracingCommandList(){};
    virtual void SetPipelineState(RHIRayTracingPipelineState* _raytracing_pso) = 0;
    virtual void TraceRay(uint32_t _width, uint32_t _height, uint32_t _depth)  = 0;
    virtual void TraceRayIndirect()                                            = 0;

    virtual void CopyBuffer(const RHICopyBufferInfo& _copy_info, RHIBuffer* _src, RHIBuffer* _dst)                            = 0;
    virtual void CopyTexture(const RHICopyTextureInfo& _copy_info, RHITexture* _src, RHITexture* _dst)                        = 0;
    virtual void CopyBufferToTexture(const RHICopyBufferToTextureInfo& _info, RHIBuffer* src_buffer, RHITexture* dst_texture) = 0;

    virtual void CopyTextureToBuffer(const RHICopyTextureToBufferInfo& _info, RHITexture* src_texture, RHIBuffer* dst_buffer) = 0;

    virtual void SetPipelineBarrier(const RHIBarrierDependencyInfo& _dependency) = 0;
};

class RHICopyCommandList : public RHICommandListBase {
public:
    virtual ~RHICopyCommandList(){};
    virtual void CopyBuffer(const RHICopyBufferInfo& _copy_info, RHIBuffer* _src, RHIBuffer* _dst)                            = 0;
    virtual void CopyTexture(const RHICopyTextureInfo& _copy_info, RHITexture* _src, RHITexture* _dst)                        = 0;
    virtual void CopyBufferToTexture(const RHICopyBufferToTextureInfo& _info, RHIBuffer* src_buffer, RHITexture* dst_texture) = 0;

    virtual void CopyTextureToBuffer(const RHICopyTextureToBufferInfo& _info, RHITexture* src_texture, RHIBuffer* dst_buffer) = 0;

    virtual void SetPipelineBarrier(const RHIBarrierDependencyInfo& _dependency) = 0;
};

enum class ECmdListType {

};

struct RHISubmitInfo;

class RHICommandQueue {
public:
    virtual ~RHICommandQueue(){};
    virtual void SubmitCommands(
        uint32_t                  _num_command_lists,
        const RHICommandListBase* _command_lists,
        const RHISubmitInfo*      _submit_info) = 0;

    virtual void WaitForQueueComplete() = 0;
};
struct RHIFenceWaitInfo {
    uint64_t               wait_value;
    RHIFence*              wait_fence;
    ERHIPipelineStageFlags wait_stage;
};

struct RHIFenceSignalInfo {
    uint64_t               signal_value;
    RHIFence*              signal_fence;
    ERHIPipelineStageFlags signal_stage;
};
struct RHISubmitInfo {

    void Wait(RHIFence* _fence, uint64_t _wait_value, ERHIPipelineStageFlags _stage = ERHIPipelineStageFlags::PS_NONE) {
        wait_infos.emplace_back(_wait_value, _fence, _stage);
    };

    void Signal(RHIFence* _fence, uint64_t _signal_value, ERHIPipelineStageFlags _stage = ERHIPipelineStageFlags::PS_NONE) {
        signal_infos.emplace_back(_signal_value, _fence, _stage);
    };

    const Moer::Array<RHIFenceWaitInfo>&   GetWaitInfos() const { return wait_infos; }
    const Moer::Array<RHIFenceSignalInfo>& GetSignalInfos() const { return signal_infos; }

private:
    Moer::Array<RHIFenceWaitInfo>   wait_infos;
    Moer::Array<RHIFenceSignalInfo> signal_infos;
};

//a unified commandlist for all usage?
class RHICmdList {
public:
    struct Impl;
};

namespace Moer {
    //used for main thread recording cmds
    class RHICommandList {
    public:
        RHICommandList();
        struct Impl;

    private:
        Moer::UniquePtr<Impl> impl;
    };
}// namespace Moer
#endif