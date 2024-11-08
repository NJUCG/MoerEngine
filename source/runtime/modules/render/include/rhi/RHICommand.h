#ifndef MOER_ENGINE_RHI_COMMAND_H
#define MOER_ENGINE_RHI_COMMAND_H

#include "math/Base.h"
#include "misc/STL.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "RenderAPI.h"
#include "shader/ShaderPipeline.h"
#include <functional>
#include <optional>
#include <string_view>
#include <type_traits>
#include <variant>
#include <io/IOCommon.h>

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
    virtual void SetPipelineState(RHIGfxPso* _graphics_pso)    = 0;
    virtual void SetPipelineState(RHIComputePso* _compute_pso) = 0;
    // virtual void Open()                                                    = 0;
    // virtual void Close()                                                   = 0;
    // virtual void Reset()                                                   = 0;
    virtual void ClearState(RHIGfxPso* _graphics_pso) = 0;

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
    virtual void TransitionTexture(RHITexture* _texture, ETextureStateFlags _usage, EPassType _dst_pass, uint8_t _mip_idx = 0, uint8_t _mip_cnt = 1) = 0;
    virtual void ExecuteTransition()                                                                                                                 = 0;
    virtual void Dispatch(uint32_t _group_count_x, uint32_t _group_count_y, uint32_t _group_count_z)                                                 = 0;

    virtual void DispatchIndirect(RHIBuffer* _buffer, uint64_t _offset) = 0;

    virtual void CopyBuffer(const RHICopyBufferInfo& _copy_info, RHIBuffer* _src, RHIBuffer* _dst)                              = 0;
    virtual void CopyTexture(const RHICopyTextureInfo& _copy_info, RHITexture* _src, RHITexture* _dst)                          = 0;
    virtual void CopyBufferToTexture(const RHICopyBufferToTextureInfo& _info, RHIBuffer* _src_buffer, RHITexture* _dst_texture) = 0;

    virtual void CopyTextureToBuffer(const RHICopyTextureToBufferInfo& _info, RHITexture* _src_texture, RHIBuffer* _dst_buffer) = 0;
    //To copy regions of a source texture into a destination texture, potentially performing format conversion, arbitrary scaling, and filtering.
    //must not be used for multi-sampled source or destination textures, use resolve instead
    virtual void BlitTexture(const RHIBlitTextureInfo& _blit_info, RHITexture* _src, RHITexture* _dst) = 0;

    //To resolve a multi-sample color texture to a non-multisample color texture
    virtual void ResolveTexture(const RHIResolveTextureInfo& _resolve_info, RHITexture* _src, RHITexture* _dst) = 0;

    virtual void SetPipelineBarrier(const RHIBarrierDependencyInfo& _dependency) = 0;

    virtual void SetCullMode(ERasterizerCullMode _cull_mode)                         = 0;
    virtual void SetPrimitiveTopology(EPrimitiveTopology _topology)                  = 0;
    virtual void SetViewPorts(uint32_t _num_viewports, const ViewPort* _p_viewports) = 0;
    virtual void SetViewPort(const ViewPort& _viewport)                              = 0;
    virtual void SetScissors(uint32_t _num_scissors, const Rect2D* _p_scissors)      = 0;
    virtual void SetScissor(const Rect2D& _scissor)                                  = 0;
    virtual void SetBlendFactors(const float _factors[4])                            = 0;
    virtual void BeginLabel(const char* _label)                                      = 0;
    virtual void EndLabel()                                                          = 0;

    virtual void BindVertexBuffers(
        uint32_t            _start_index,
        uint32_t            _num_buffers,
        const RHIBufferRef* _p_vertex_buffers,
        const uint32_t*     _offsets) = 0;

    virtual void BindIndexBuffer(
        const RHIBuffer*  _p_index_buffer,
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

    virtual void BindParameters(Shader* _shader, RHIBatchedShaderParameters* _batched_params) {};
};

class RHIComputeCommandList : public RHICommandListBase {
public:
    virtual ~RHIComputeCommandList(){};
    virtual void SetPipelineState(RHIComputePso* _compute_pso)                                       = 0;
    virtual void Dispatch(uint32_t _group_count_x, uint32_t _group_count_y, uint32_t _group_count_z) = 0;
    virtual void DispatchIndirect(RHIBuffer* _buffer, uint64_t _offset)                              = 0;

    virtual void CopyBuffer(const RHICopyBufferInfo& _copy_info, RHIBuffer* _src, RHIBuffer* _dst)                              = 0;
    virtual void CopyTexture(const RHICopyTextureInfo& _copy_info, RHITexture* _src, RHITexture* _dst)                          = 0;
    virtual void CopyBufferToTexture(const RHICopyBufferToTextureInfo& _info, RHIBuffer* _src_buffer, RHITexture* _dst_texture) = 0;

    virtual void CopyTextureToBuffer(const RHICopyTextureToBufferInfo& _info, RHITexture* _src_texture, RHIBuffer* _dst_buffer) = 0;

    virtual void SetPipelineBarrier(const RHIBarrierDependencyInfo& _dependency) = 0;
};

class RHIRayTracingCommandList : public RHICommandListBase {
public:
    virtual ~RHIRayTracingCommandList(){};
    virtual void SetPipelineState(RHIRTPso* _raytracing_pso)                  = 0;
    virtual void TraceRay(uint32_t _width, uint32_t _height, uint32_t _depth) = 0;
    virtual void TraceRayIndirect()                                           = 0;

    virtual void CopyBuffer(const RHICopyBufferInfo& _copy_info, RHIBuffer* _src, RHIBuffer* _dst)                              = 0;
    virtual void CopyTexture(const RHICopyTextureInfo& _copy_info, RHITexture* _src, RHITexture* _dst)                          = 0;
    virtual void CopyBufferToTexture(const RHICopyBufferToTextureInfo& _info, RHIBuffer* _src_buffer, RHITexture* _dst_texture) = 0;

    virtual void CopyTextureToBuffer(const RHICopyTextureToBufferInfo& _info, RHITexture* _src_texture, RHIBuffer* _dst_buffer) = 0;

    virtual void SetPipelineBarrier(const RHIBarrierDependencyInfo& _dependency) = 0;
};

class RHICopyCommandList : public RHICommandListBase {
public:
    virtual ~RHICopyCommandList(){};
    virtual void CopyBuffer(const RHICopyBufferInfo& _copy_info, RHIBuffer* _src, RHIBuffer* _dst)                              = 0;
    virtual void CopyTexture(const RHICopyTextureInfo& _copy_info, RHITexture* _src, RHITexture* _dst)                          = 0;
    virtual void CopyBufferToTexture(const RHICopyBufferToTextureInfo& _info, RHIBuffer* _src_buffer, RHITexture* _dst_texture) = 0;

    virtual void CopyTextureToBuffer(const RHICopyTextureToBufferInfo& _info, RHITexture* _src_texture, RHIBuffer* _dst_buffer) = 0;

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

namespace Moer::Render {
    enum class EQueueType : uint8 {
        Graphics,
        Compute,
        Copy,
        Num,
        Ignore
    };

    struct Command {
    public:
        enum class EType {
            UploadBuffer,
            CopyBackBuffer,
            BufferToBuffer,
            BufferToTexture,
            TextureToBuffer,
            UploadTexture,
            TextureToTexture,
            ShaderDispatch,
            BuildAccel,
            BuildTLAS,
            TraceRay,
            Barrier,
            QueueTransfer,
            SetDrawState,
            UpdateBindlessArray,
            // UpdateDrawState,
            // SetParams,
            // SetConstants,
            Custom
        };

        static constexpr std::string_view typenames[] = {
            "UploadBuffer",
            "CopyBackBuffer",
            "BufferToBuffer",
            "BufferToTexture",
            "TextureToBuffer",
            "UploadTexture",
            "TextureToTexture",
            "ShaderDispatch",
            "BuildAccel",
            "BuildTLAS",
            "TraceRay",
            "Barrier",
            "QueueTransfer",
            "SetDrawState",
            "UpdateBindlessArray",
            "Custom"};

    private:
        EType type;

    public:
        explicit Command(EType _type) : type(_type), name(typenames[uint(_type)]) {}
        explicit Command(EType _type, std::string_view _name) : type(_type), name(_name) {}
        virtual ~Command()                      = default;
        virtual EQueueType GetQueueType() const = 0;

    public:
        EType       Type() const { return type; }
        std::string name;
    };

    struct WaitEvent {
        uint64 timeline_handle;
        uint64 value;
    };
    struct SignalEvent {
        uint64 timeline_handle;
        uint64 value;
    };
    template<typename TRenderTarget>
    concept is_render_target = std::is_same_v<TRenderTarget, ColorAttachment>;

    struct VertexBuffer {
        Buffer* buffer;
        uint64  offset{0};
    };
    struct IndexBuffer {
        BufferView        buffer;
        EIndexElementType stride;
    };

    struct SingleDrawParam {
        uint index_cnt;
        uint instance_cnt;
        uint first_index;
        uint vertex_offset;
        uint first_instance;
    };
    struct MeshDrawData {
        StaticArray<VertexBuffer, 4>    vtx_views;
        std::variant<IndexBuffer, uint> idx_view = 0u;
        uint                            vtx_cnt  = 0;

        Array<SingleDrawParam> draw_params;

    public:
        MeshDrawData() = default;
        MeshDrawData(MeshDrawData&& _other) noexcept {
            vtx_views   = std::move(_other.vtx_views);
            idx_view    = std::move(_other.idx_view);
            draw_params = std::move(_other.draw_params);
        }
        MeshDrawData& operator=(MeshDrawData&& _other) noexcept {
            vtx_views   = std::move(_other.vtx_views);
            idx_view    = std::move(_other.idx_view);
            draw_params = std::move(_other.draw_params);
            return *this;
        }

        MeshDrawData(
            std::span<VertexBuffer> _vertex_buffers,
            IndexBuffer             _index_buffer) : idx_view(_index_buffer) {
            vtx_views.fill({nullptr, 0});
            vtx_cnt = _vertex_buffers.size();
            memcpy(vtx_views.data(), _vertex_buffers.data(), _vertex_buffers.size() * sizeof(VertexBuffer));
        }

        MeshDrawData(
            std::span<VertexBuffer> _vtx_views,
            uint                    _vtx_cnt) : idx_view(_vtx_cnt) {
            vtx_views.fill({nullptr, 0});
            memcpy(vtx_views.data(), _vtx_views.data(), _vtx_views.size() * sizeof(VertexBuffer));
        }

        void EmplaceDrawIndexed(
            uint _first_index,
            uint _index_cnt,
            uint _first_vertex,
            uint _first_instance,
            uint _instance_cnt = 1) {

            draw_params.emplace_back(SingleDrawParam{_index_cnt, _instance_cnt, _first_index, _first_vertex, _first_instance});
        }

        void EmplaceDraw(
            uint _vertex_cnt,
            uint _first_vertex,
            uint _first_instance,
            uint _instance_cnt = 1) {

            draw_params.emplace_back(SingleDrawParam{_vertex_cnt, _instance_cnt, 0, _first_vertex, _first_instance});
        }
        void Reserve(uint _size) {
            draw_params.reserve(_size);
        }
    };
    struct CmdSubmit {
        Array<UniquePtr<Command>>        cmds;
        Array<std::function<void(void)>> callbacks;
        BindlessArrayRef                 bindless_array{nullptr};

        Array<WaitEvent>   wait_events;
        Array<SignalEvent> signal_events;
        bool               b_sync{false};//force sync queue timeline
        CmdSubmit&&        Wait(Fence* _fence, uint64 _wait_value) {
            wait_events.emplace_back(uint64(_fence), _wait_value);
            return std::move(*this);
        }

        CmdSubmit&& Wait(WaitEvent _event) {
            wait_events.emplace_back(_event);
            return std::move(*this);
        }

        CmdSubmit&& Signal(Fence* _fence, uint64 _signal_value) {
            signal_events.emplace_back(uint64(_fence), _signal_value);
            return std::move(*this);
        }

        CmdSubmit(CmdSubmit&& _other) noexcept {
            cmds          = std::move(_other.cmds);
            callbacks     = std::move(_other.callbacks);
            wait_events   = std::move(_other.wait_events);
            signal_events = std::move(_other.signal_events);
            b_sync        = _other.b_sync;
        }

        CmdSubmit& operator=(CmdSubmit&& _other) noexcept {
            cmds          = std::move(_other.cmds);
            callbacks     = std::move(_other.callbacks);
            wait_events   = std::move(_other.wait_events);
            signal_events = std::move(_other.signal_events);
            b_sync        = _other.b_sync;
            return *this;
        }
        CmdSubmit(Array<UniquePtr<Command>>&& _cmds, Array<std::function<void(void)>>&& _callbacks, BindlessArrayRef _bindless_array) : cmds(std::move(_cmds)), callbacks(std::move(_callbacks)), bindless_array(_bindless_array) {
        }
    };

    struct ReadTexture {
        TextureView   texture;
        ETextureState state;
    };
    struct WriteTexture {
        TextureView   texture;
        ETextureState state;
    };

    struct ReadBuffer {
        BufferView   buffer;
        EBufferState state;
    };

    struct WriteBuffer {
        BufferView   buffer;
        EBufferState state;
    };

    struct ImportTexture {
        TextureView   texture;
        ETextureState state;
    };

    struct ExportTexture {
        TextureView   texture;
        ETextureState state;
    };

    struct ImportBuffer {
        BufferView   buffer;
        EBufferState state;
    };

    struct ExportBuffer {
        BufferView   buffer;
        EBufferState state;
    };

    template<typename TInArg>
    concept is_arg =
        std::is_same_v<std::remove_reference_t<TInArg>(), BufferView> || std::is_same_v<std::remove_reference_t<TInArg>(), TextureView> || std::is_same_v<std::remove_reference_t<TInArg>(), Buffer*> || std::is_same_v<std::remove_reference_t<TInArg>(), Texture*>;
    class CommandList {
    public:
        struct RENDER_API DrawDispatcher {
            DrawDispatcher(RasterPipeline& _pso, CommandList& _cmd_list);

            DrawDispatcher(RasterPipeline& _pso, CommandList& _cmd_list, ArrayArguments&& _args);

            template<typename... TRenderTarget>
            void Draw(Rect2D _rect, Array<MeshDrawData>&& _mesh_data, DepthAttachment _depth, TRenderTarget&&... _render_targets) {
                RenderPassInfo pass_info(
                    {std::forward<TRenderTarget>(_render_targets)...},
                    _depth,
                    _rect);
                cmd_list.SetRenderCmds(pso.handle, std::move(args), std::move(pass_info), std::move(_mesh_data));
            };

            template<typename... TRenderTarget>
            void Draw(Rect2D _rect, std::span<VertexBuffer> _vtx, IndexBuffer _idx, Array<SingleDrawParam>&& _mesh_data, DepthAttachment _depth, TRenderTarget&&... _render_targets) {

                Array<MeshDrawData> mesh_data;
                mesh_data.emplace_back(_vtx, _idx);
                mesh_data.back().draw_params = std::move(_mesh_data);

                Draw(_rect, std::move(mesh_data), _depth, std::forward<TRenderTarget>(_render_targets)...);
            };

            template<typename... TRenderTarget>
            void Draw(Rect2D _rect, std::span<VertexBuffer> _vtx, IndexBuffer _idx, Array<SingleDrawParam>&& _mesh_data, TRenderTarget&&... _render_targets) {

                Array<MeshDrawData> mesh_data;
                mesh_data.emplace_back(_vtx, _idx);
                mesh_data.back().draw_params = std::move(_mesh_data);

                Draw(_rect, std::move(mesh_data), std::forward<TRenderTarget>(_render_targets)...);
            };

            template<typename... TRenderTarget>
            void Draw(Rect2D _rect, std::span<VertexBuffer> _vtx, uint _vtx_cnt, Array<SingleDrawParam>&& _mesh_data, DepthAttachment _depth, TRenderTarget&&... _render_targets) {
                RenderPassInfo pass_info(
                    {std::forward<TRenderTarget>(_render_targets)...},
                    _depth,
                    _rect);
                Array<MeshDrawData> mesh_data;
                mesh_data.emplace_back(_vtx, _vtx_cnt);
                mesh_data.back().draw_params = std::move(_mesh_data);

                cmd_list.SetRenderCmds(pso.handle, std::move(args), std::move(pass_info), std::move(mesh_data));
            };

            template<typename... TRenderTarget>
            void Draw(Rect2D _rect, Array<MeshDrawData>&& _mesh_data, TRenderTarget&&... _render_targets) {
                Draw(_rect, std::move(_mesh_data), DepthAttachment{}, std::forward<TRenderTarget>(_render_targets)...);
            };

            //draw with names
            template<typename... TRenderTarget>
            void Draw(std::string_view _name, Rect2D _rect, Array<MeshDrawData>&& _mesh_data, DepthAttachment _depth, TRenderTarget&&... _render_targets) {
                RenderPassInfo pass_info(
                    {std::forward<TRenderTarget>(_render_targets)...},
                    _depth,
                    _rect);
                cmd_list.SetRenderCmds(pso.handle, std::move(args), std::move(pass_info), std::move(_mesh_data), _name);
            };

            template<typename... TRenderTarget>
            void Draw(std::string_view _name, Rect2D _rect, std::span<VertexBuffer> _vtx, IndexBuffer _idx, Array<SingleDrawParam>&& _mesh_data, DepthAttachment _depth, TRenderTarget&&... _render_targets) {

                Array<MeshDrawData> mesh_data;
                mesh_data.emplace_back(_vtx, _idx);
                mesh_data.back().draw_params = std::move(_mesh_data);

                Draw(_name, _rect, std::move(mesh_data), _depth, std::forward<TRenderTarget>(_render_targets)...);
            };

            template<typename... TRenderTarget>
            void Draw(std::string_view _name, Rect2D _rect, std::span<VertexBuffer> _vtx, IndexBuffer _idx, Array<SingleDrawParam>&& _mesh_data, TRenderTarget&&... _render_targets) {

                Array<MeshDrawData> mesh_data;
                mesh_data.emplace_back(_vtx, _idx);
                mesh_data.back().draw_params = std::move(_mesh_data);

                Draw(_name, _rect, std::move(mesh_data), std::forward<TRenderTarget>(_render_targets)...);
            };

            template<typename... TRenderTarget>
            void Draw(std::string_view _name, Rect2D _rect, std::span<VertexBuffer> _vtx, uint _vtx_cnt, Array<SingleDrawParam>&& _mesh_data, DepthAttachment _depth, TRenderTarget&&... _render_targets) {
                RenderPassInfo pass_info(
                    {std::forward<TRenderTarget>(_render_targets)...},
                    _depth,
                    _rect);
                Array<MeshDrawData> mesh_data;
                mesh_data.emplace_back(_vtx, _vtx_cnt);
                mesh_data.back().draw_params = std::move(_mesh_data);

                cmd_list.SetRenderCmds(pso.handle, std::move(args), std::move(pass_info), std::move(mesh_data), _name);
            };

            template<typename... TRenderTarget>
            void Draw(std::string_view _name, Rect2D _rect, std::span<VertexBuffer> _vtx, uint _vtx_cnt, Array<SingleDrawParam>&& _mesh_data, TRenderTarget&&... _render_targets) {
                RenderPassInfo pass_info(
                    {std::forward<TRenderTarget>(_render_targets)...},
                    DepthAttachment{},
                    _rect);
                Array<MeshDrawData> mesh_data;
                mesh_data.emplace_back(_vtx, _vtx_cnt);
                mesh_data.back().draw_params = std::move(_mesh_data);

                cmd_list.SetRenderCmds(pso.handle, std::move(args), std::move(pass_info), std::move(mesh_data), _name);
            };

            template<typename... TRenderTarget>
            void Draw(std::string_view _name, Rect2D _rect, Array<MeshDrawData>&& _mesh_data, TRenderTarget&&... _render_targets) {
                Draw(_name, _rect, std::move(_mesh_data), DepthAttachment{}, std::forward<TRenderTarget>(_render_targets)...);
            };

            RasterPipeline& pso;
            CommandList&    cmd_list;
            ArrayArguments  args;

        private:
            // void SubmitArgsIfPossible();

            // bool HasParams() const {
            //     return b_set_params;
            // }
            // ArgSetter arg_setter;
            // bool      b_set_params = false;
            // bool      b_set_consts = false;
        };

        struct RENDER_API RaytracingDispatcher {
            CommandList&   cmd_list;
            ArrayArguments args;
            RTPipeline&    pso;
        };
        struct RENDER_API ComputeDispatcher {
            void Dispatch(Vector2ui _group_count, std::string_view _name = Command::typenames[(uint)Command::EType::ShaderDispatch]) {
                Dispatch(uint3(_group_count, 1), _name);
            }
            void Dispatch(Vector3ui _group_count, std::string_view _name = Command::typenames[(uint)Command::EType::ShaderDispatch]);
            void Dispatch(uint _group_cnt, std::string_view _name = Command::typenames[(uint)Command::EType::ShaderDispatch]) {
                Dispatch(Vector3ui(_group_cnt, 1, 1), _name);
            }
            void DispatchIndirect(BufferView, std::string_view _name = Command::typenames[(uint)Command::EType::ShaderDispatch]);
            ComputeDispatcher(ComputePipeline& _pso, CommandList& _cmd_list, ArrayArguments&& _args);
            ComputeDispatcher(ComputePipeline& _pso, CommandList& _cmd_list);
            // template<typename T>
            // ComputeDispatcher& SetParam(std::string_view _name, T&& _param) {
            //     arg_setter.SetParam(_name, std::forward<T>(_param));
            //     b_set_params = true;
            //     return *this;
            // }
            // template<typename T>
            // ComputeDispatcher& SetConstant(T&& _param) {
            //     arg_setter.SetConstant(std::forward<T>(_param));
            //     b_set_consts = true;
            //     return *this;
            // }
            ComputePipeline& pso;
            CommandList&     cmd_list;
            ArrayArguments   args;

        private:
            // void SubmitArgsIfPossible();

            // bool HasParams() const {
            //     return b_set_params;
            // }
            // ArgSetter arg_setter;
            // bool      b_set_params = false;
            // bool      b_set_consts = false;
        };
        friend class VkCommandQueue;

        using Dispatcher = std::variant<DrawDispatcher, ComputeDispatcher>;

    public:
        RENDER_API CommandList();
        class Impl;

        template<typename TGfxPso, typename... TArgs>
        DrawDispatcher Gfx(TGfxPso& _pso, TArgs&&... _args) {
            if constexpr (sizeof...(TArgs) > 0) {
                ArrayArguments&& args = _pso.SetArgs(_args...);
                return DrawDispatcher(_pso, *this, std::move(args));
            }
            return DrawDispatcher(_pso, *this);
        }

        template<typename TComputePso, typename... TArgs>
        ComputeDispatcher Compute(TComputePso& _pso, TArgs&&... _args) {
            if constexpr (sizeof...(TArgs) > 0) {
                ArrayArguments&& args = _pso.SetArgs(_args...);
                return ComputeDispatcher(_pso, *this, std::move(args));
            }
            return ComputeDispatcher(_pso, *this);
        };

        template<typename TRTPso, typename... TArgs>
        void RT(TRTPso& _pso, TArgs&&... _args) {
            // ArrayArguments&& args = std::move(_pso.SetArgs(_args...));
            // commands.push_back(MakeUnique<ShaderDispatchCmd>(_pso, std::move(args)));
        }

        RENDER_API void CopyFrom(BufferView _src, BufferView _dst, std::string_view _name = Command::typenames[(uint)Command::EType::BufferToBuffer]);
        RENDER_API void CopyFrom(TextureView _src, TextureView _dst, std::string_view _name = Command::typenames[(uint)Command::EType::TextureToTexture]);
        RENDER_API void CopyFrom(TextureView _src, BufferView _dst, std::string_view _name = Command::typenames[(uint)Command::EType::TextureToBuffer]);
        RENDER_API void CopyFrom(BufferView _src, TextureView _dst, std::string_view _name = Command::typenames[(uint)Command::EType::BufferToTexture]);
        RENDER_API void CopyFrom(std::span<byte> _data, BufferView _dst, std::string_view _name = Command::typenames[(uint)Command::EType::UploadBuffer]);
        RENDER_API void CopyFrom(std::span<byte> _data, TextureView _dst, std::string_view _name = Command::typenames[(uint)Command::EType::UploadTexture]);
        RENDER_API void CopyFrom(Array<byte>&& _data, BufferView _dst, std::string_view _name = Command::typenames[(uint)Command::EType::UploadBuffer]);
        RENDER_API void CopyFrom(Array<byte>&& _data, TextureView _dst, std::string_view _name = Command::typenames[(uint)Command::EType::UploadTexture]);
        RENDER_API void CopyFrom(BufferView _src, std::span<byte> _data, std::string_view _name = Command::typenames[(uint)Command::EType::CopyBackBuffer]);

        RENDER_API void UpdateBindlessArray(BindlessArrayRef _array);

        template<typename T, typename... Args>
        struct CountType;

        template<typename T>
        struct CountType<T> {
            static constexpr uint32_t value = 0;
        };

        template<typename T, typename First, typename... Rest>
        struct CountType<T, First, Rest...> {
            static constexpr uint32_t value = std::is_same_v<T, First> + CountType<T, Rest...>::value;
        };

        template<typename... Args>
        struct GetReadTextureCnt {
            static constexpr uint32_t value = CountType<ReadTexture, Args...>::value;
        };

        template<typename... Args>
        struct GetWriteTextureCnt {
            static constexpr uint32_t value = CountType<WriteTexture, Args...>::value;
        };

        template<typename... Args>
        struct GetReadBufferCnt {
            static constexpr uint32_t value = CountType<ReadBuffer, Args...>::value;
        };

        template<typename... Args>
        struct GetWriteBufferCnt {
            static constexpr uint32_t value = CountType<WriteBuffer, Args...>::value;
        };

        template<typename... T>
        void Barriers(EQueueType _src_queue, EQueueType _dst_queue, EPassType _pass, T... _args) {
            constexpr uint read_tex_cnt  = GetReadTextureCnt<T...>::value;
            constexpr uint write_tex_cnt = GetWriteTextureCnt<T...>::value;
            constexpr uint read_buf_cnt  = GetReadBufferCnt<T...>::value;
            constexpr uint write_buf_cnt = GetWriteBufferCnt<T...>::value;
            static_assert(read_tex_cnt + write_tex_cnt + read_buf_cnt + write_buf_cnt > 0, "no barriers");

            BeginBarriers(read_tex_cnt, write_tex_cnt, read_buf_cnt, write_buf_cnt, _src_queue, _dst_queue);
            (InnerBarrier(_args, _pass), ...);
            EndBarriers();
        }

        void TextureBarriers(EQueueType _src_queue, EQueueType _dst_queue, EPassType _pass, Array<ReadTexture>&& _read_tex, Array<WriteTexture>&& _write_tex) {
            BeginBarriers(_read_tex.size(), _write_tex.size(), 0, 0, _src_queue, _dst_queue);
            for (auto& tex : _read_tex) {
                InnerBarrier(tex, _pass);
            }
            for (auto& tex : _write_tex) {
                InnerBarrier(tex, _pass);
            }
            EndBarriers();
        }

        void BufferBarriers(EQueueType _src_queue, EQueueType _dst_queue, EPassType _pass, Array<ReadBuffer>&& _read_buf, Array<WriteBuffer>&& _write_buf) {
            BeginBarriers(0, 0, _read_buf.size(), _write_buf.size(), _src_queue, _dst_queue);
            for (auto& buf : _read_buf) {
                InnerBarrier(buf, _pass);
            }
            for (auto& buf : _write_buf) {
                InnerBarrier(buf, _pass);
            }
            EndBarriers();
        }

        void TextureBarriers(EQueueType _src_queue, EQueueType _dst_queue, EPassType _pass, Array<ReadTexture>&& _read_tex) {
            BeginBarriers(_read_tex.size(), 0, 0, 0, _src_queue, _dst_queue);
            for (auto& tex : _read_tex) {
                InnerBarrier(tex, _pass);
            }
            EndBarriers();
        }

        void TextureBarriers(EQueueType _src_queue, EQueueType _dst_queue, EPassType _pass, Array<WriteTexture>&& _write_tex) {
            BeginBarriers(0, _write_tex.size(), 0, 0, _src_queue, _dst_queue);
            for (auto& tex : _write_tex) {
                InnerBarrier(tex, _pass);
            }
            EndBarriers();
        }

        RENDER_API void ImportTextureFromQueue(EQueueType _src_queue, Array<ImportTexture>&& _textures_to_import);
        RENDER_API void ExportTextureToQueue(EQueueType _dst_queue, Array<ExportTexture>&& _textures_to_export);

#pragma region[ raytracing ]

        RENDER_API void BuildAccelerationStructures(Array<AccelerationStructureBuildParam>&& _params);

        RENDER_API void UpdateRaytracingScene(RaytracingSceneRef _scene);

#pragma endregion

        RENDER_API void AddCallback(std::function<void()>&& _callback);

        RENDER_API CmdSubmit Submit();

    private:
        friend DrawDispatcher;
        friend ComputeDispatcher;
        friend class CommandQueue;
        RENDER_API void SetRenderCmds(PipelineHandle& _handle, ArrayArguments&& _args, RenderPassInfo&&, Array<MeshDrawData>&&, std::optional<std::string_view> _name = std::nullopt);
        // void SubmitArgs(ShaderPipeline&, Arguments&&);
        // void SubmitConstants(ShaderPipeline&, Array<uint>&&);

        RENDER_API void BeginBarriers(uint _read_tex_cnt, uint _write_tex_cnt, uint _read_buf_cnt, uint _write_buf_cnt, EQueueType _src_queue, EQueueType _dst_queue);
        RENDER_API void InnerBarrier(ReadBuffer _buffer, EPassType _pass) {
            InnerReadBuffer(_buffer.buffer, _buffer.state, _pass);
        }
        RENDER_API void InnerBarrier(WriteBuffer _buffer, EPassType _pass) {
            InnerWriteBuffer(_buffer.buffer, _buffer.state, _pass);
        }
        RENDER_API void InnerBarrier(ReadTexture _texture, EPassType _pass) {
            InnerReadTexture(_texture.texture, _texture.state, _pass);
        }
        RENDER_API void InnerBarrier(WriteTexture _texture, EPassType _pass) {
            InnerWriteTexture(_texture.texture, _texture.state, _pass);
        }

        RENDER_API void InnerReadBuffer(BufferView _buffer, EBufferState _state, EPassType _pass);
        RENDER_API void InnerWriteBuffer(BufferView _buffer, EBufferState _state, EPassType _pass);
        RENDER_API void InnerReadTexture(TextureView _texture, ETextureState _state, EPassType _pass);
        RENDER_API void InnerWriteTexture(TextureView _texture, ETextureState _state, EPassType _pass);
        RENDER_API void EndBarriers();

#pragma region[ raytracing ]
        RENDER_API void TraceRays(PipelineHandle _pipeline, ArrayArguments&& _args, uint3 _extent);
        RENDER_API void TraceRayIndirect(PipelineHandle _pipeline, BufferView _buffer);
#pragma endregion

        Array<UniquePtr<Command>>    commands;
        Command*                     current_barriers{nullptr};
        Array<std::function<void()>> callbacks;
        BindlessArrayRef             bindless_array{nullptr};
    };
    class QueueCmd {};

    class RENDER_API CommandQueue {
    public:
        CommandQueue(){};
        CommandQueue(EQueueType _type, RenderDevice& _device);
        void              Test();
        virtual void      Wait(WaitEvent _event)                                = 0;
        virtual WaitEvent Execute(CmdSubmit&& _submit)                          = 0;
        virtual void      Present(SwapchainRef _swapchain, TextureView _target) = 0;
        virtual void      Sync()                                                = 0;
    };

    class RENDER_API CopyQueue {
    public:
        CopyQueue(){};
        ~CopyQueue()                                      = default;
        virtual IOWaitEvt Execute(IOSubmission&& _submit) = 0;
        virtual IOWaitEvt Execute(CmdSubmit&& _submit)    = 0;

        virtual void CopyFrom(BufferView _src, BufferView _dst)        = 0;
        virtual void CopyFrom(TextureView _src, TextureView _dst)      = 0;
        virtual void CopyFrom(TextureView _src, BufferView _dst)       = 0;
        virtual void CopyFrom(BufferView _src, TextureView _dst)       = 0;
        virtual void CopyFrom(std::span<byte> _data, BufferView _dst)  = 0;
        virtual void CopyFrom(std::span<byte> _data, TextureView _dst) = 0;

        virtual FenceRef GetFenceHandle()       = 0;
        virtual void     Sync(uint64 _timeline) = 0;
    };
}// namespace Moer::Render
#endif