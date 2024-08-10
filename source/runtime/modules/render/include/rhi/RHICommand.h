#ifndef MOER_ENGINE_RHI_COMMAND_H
#define MOER_ENGINE_RHI_COMMAND_H

#include "math/Base.h"
#include "misc/STL.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "RenderAPI.h"
#include "shader/ShaderPipeline.h"
#include <functional>
#include <type_traits>
#include <variant>

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

    virtual void BindParameters(Shader* _shader, RHIBatchedShaderParameters* _batched_params){};
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
    enum class EQueueType {
        Graphics,
        Compute,
        Copy,
        Num
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
            Barrier,
            SetDrawState,
            UpdateDrawState,
            Draw,
            SetParams,
            SetConstants,
            Dispatch,
            Custom
        };

    private:
        EType type;

    public:
        explicit Command(EType _type) : type(_type) {}
        virtual ~Command()                      = default;
        virtual EQueueType GetQueueType() const = 0;

    public:
        EType Type() const { return type; }
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
    struct MeshDrawData {
        StaticArray<VertexBuffer, 4>    vtx_views;
        std::variant<IndexBuffer, uint> idx_view;
        uint                            instance_count{1};
        uint                            instance_offset{0};
        uint                            vtx_cnt;

    public:
        MeshDrawData() = default;
        MeshDrawData(MeshDrawData&& _other) noexcept {
            vtx_views       = std::move(_other.vtx_views);
            idx_view        = std::move(_other.idx_view);
            instance_count  = _other.instance_count;
            instance_offset = _other.instance_offset;
        }
        MeshDrawData& operator=(MeshDrawData&& _other) noexcept {
            vtx_views       = std::move(_other.vtx_views);
            idx_view        = std::move(_other.idx_view);
            instance_count  = _other.instance_count;
            instance_offset = _other.instance_offset;
            return *this;
        }

        MeshDrawData(
            std::span<VertexBuffer> _vertex_buffers,
            IndexBuffer             _index_buffer,
            uint                    _instance_count,
            uint                    _instance_offset) : idx_view(_index_buffer),
                                     instance_count(_instance_count),
                                     instance_offset(_instance_offset) {
            vtx_views.fill({nullptr, 0});
            vtx_cnt = _vertex_buffers.size();
            memcpy(vtx_views.data(), _vertex_buffers.data(), _vertex_buffers.size() * sizeof(VertexBuffer));
        }

        MeshDrawData(
            std::span<VertexBuffer> _vtx_views,
            uint                    _vtx_cnt,
            uint                    _instance_count,
            uint                    _instance_offset) : instance_count(_instance_count),
                                     idx_view(_vtx_cnt),
                                     instance_offset(_instance_offset) {
            vtx_views.fill({nullptr, 0});
            memcpy(vtx_views.data(), _vtx_views.data(), _vtx_views.size() * sizeof(VertexBuffer));
        }
    };
    struct CmdSubmit {
        Array<UniquePtr<Command>>        cmds;
        Array<std::function<void(void)>> callbacks;

        Array<WaitEvent>   wait_events;
        Array<SignalEvent> signal_events;
        bool               b_sync{false};//force sync queue timeline
        CmdSubmit&         Wait(Fence* _fence, uint64 _wait_value) {
            wait_events.emplace_back(uint64(_fence), _wait_value);
            return *this;
        }

        CmdSubmit& Signal(Fence* _fence, uint64 _signal_value) {
            signal_events.emplace_back(uint64(_fence), _signal_value);
            return *this;
        }
    };
    template<typename TInArg>
    concept is_arg =
        std::is_same_v<std::remove_reference_t<TInArg>(), BufferView> || std::is_same_v<std::remove_reference_t<TInArg>(), TextureView> || std::is_same_v<std::remove_reference_t<TInArg>(), Buffer*> || std::is_same_v<std::remove_reference_t<TInArg>(), Texture*>;
    class CommandList {
    public:
        struct RENDER_API ArgSetter {
        public:
            ArgSetter(ShaderPipeline& _handle) : handle(_handle) {
            }

            template<is_arg T>
            void SetParam(std::string_view _name, T&& _param) {
                using Type = std::remove_reference_t<T>();
                if constexpr (std::is_same_v<Type, BufferView>) {
                    SetBuffer(std::hash<std::string_view>{}(_name), _param);
                } else if constexpr (std::is_same_v<Type, TextureView>) {
                    SetTexture(std::hash<std::string_view>{}(_name), _param);
                } else if constexpr (std::is_same_v<Type, Buffer*>) {
                    assert(_param && "buffer is nullptr");
                    SetBuffer(std::hash<std::string_view>{}(_name), _param->GetView());
                } else if constexpr (std::is_same_v<Type, Texture*>) {

                    assert(_param && "texture is nullptr");
                    SetTexture(std::hash<std::string_view>{}(_name), _param->GetView());
                } else {
                    // static_assert(false, "unsupported type");
                    assert(0 && "unsupported type");
                }
            }
            template<typename T>
            void SetConstant(T&& _param) {
                SetConstant(&_param, sizeof(T));
            }
            Arguments&& StealArgs() {
                return std::move(temp_args);
            }
            Array<uint>&& StealConstants() {
                return std::move(temp_constant);
            }

        private:
            void SetBuffer(uint64 _hash, BufferView _buffer);
            void SetTexture(uint64 _hash, TextureView _texture);
            void SetConstant(void*, uint _size);

            Arguments       temp_args;
            Array<uint>     temp_constant;
            ShaderPipeline& handle;
        };
        struct RENDER_API DrawDispatcher {
            DrawDispatcher(RasterPipeline& _pso, CommandList& _cmd_list);

            DrawDispatcher(RasterPipeline& _pso, CommandList& _cmd_list, ArrayArguments&& _args);

            template<typename T>
            DrawDispatcher& SetParam(std::string_view _name, T&& _param) {
                arg_setter.SetParam(_name, std::forward<T>(_param));
                b_set_params = true;
            }
            template<typename T>
            DrawDispatcher& SetConstant(T&& _param) {
                arg_setter.SetConstant(std::forward<T>(_param));
                b_set_consts = true;
                return *this;
            }
            template<typename... TRenderTarget>
            void Draw(Rect2D _rect, Array<MeshDrawData>&& _mesh_data, DepthAttachment _depth, TRenderTarget&&... _render_targets) {
                RenderPassInfo pass_info(
                    {std::forward<TRenderTarget>(_render_targets)...},
                    _depth,
                    _rect);
                cmd_list.SetRenderCmds(pso.handle, std::move(pass_info), std::move(_mesh_data));
            };

            template<typename... TRenderTarget>
            void Draw(Rect2D _rect, Array<MeshDrawData>&& _mesh_data, TRenderTarget&&... _render_targets) {
                Draw(_rect, std::move(_mesh_data), DepthAttachment{}, std::forward<TRenderTarget>(_render_targets)...);
            };

            RasterPipeline& pso;
            CommandList&    cmd_list;

        private:
            void SubmitArgsIfPossible();

            bool HasParams() const {
                return b_set_params;
            }
            ArgSetter arg_setter;
            bool      b_set_params = false;
            bool      b_set_consts = false;
        };
        struct RENDER_API ComputeDispatcher {
            void Dispatch(Vector2ui _group_count) {
                Dispatch(uint3(_group_count, 1));
            }
            void Dispatch(Vector3ui _group_count);
            void Dispatch(uint _group_cnt) {
                Dispatch(Vector3ui(_group_cnt, 1, 1));
            }
            void DispatchIndirect(BufferView);
            ComputeDispatcher(ComputePipeline& _pso, CommandList& _cmd_list, ArrayArguments&& _args);
            ComputeDispatcher(ComputePipeline& _pso, CommandList& _cmd_list);
            template<typename T>
            ComputeDispatcher& SetParam(std::string_view _name, T&& _param) {
                arg_setter.SetParam(_name, std::forward<T>(_param));
                b_set_params = true;
                return *this;
            }
            template<typename T>
            ComputeDispatcher& SetConstant(T&& _param) {
                arg_setter.SetConstant(std::forward<T>(_param));
                b_set_consts = true;
                return *this;
            }
            ComputePipeline& pso;
            CommandList&     cmd_list;

        private:
            void SubmitArgsIfPossible();

            bool HasParams() const {
                return b_set_params;
            }
            ArgSetter arg_setter;
            bool      b_set_params = false;
            bool      b_set_consts = false;
        };
        friend class VkCommandQueue;

        using Dispatcher = std::variant<DrawDispatcher, ComputeDispatcher>;

    public:
        RENDER_API CommandList();
        class Impl;

        template<typename TGfxPso, typename... TArgs>
        DrawDispatcher Gfx(TGfxPso& _pso, TArgs&&... _args) {
            if constexpr (sizeof...(TArgs) > 0) {
                // ArrayArguments&& args = std::move(_pso.SetArgs());
                return DrawDispatcher(_pso, *this, std::move(_pso.SetArgs(_args...)));
            }
            return DrawDispatcher(_pso, *this);
        }

        template<typename TComputePso, typename... TArgs>
        ComputeDispatcher Compute(TComputePso& _pso, TArgs&&... _args) {
            if constexpr (sizeof...(TArgs) > 0) {
                ArrayArguments&& args = std::move(_pso.SetArgs());
                return ComputeDispatcher(_pso, *this, std::move(args));
            }
            return ComputeDispatcher(_pso, *this);
        };

        RENDER_API void CopyFrom(BufferView _src, BufferView _dst);
        RENDER_API void CopyFrom(TextureView _src, TextureView _dst);
        RENDER_API void CopyFrom(TextureView _src, BufferView _dst);
        RENDER_API void CopyFrom(BufferView _src, TextureView _dst);
        RENDER_API void CopyFrom(std::span<byte> _data, BufferView _dst);
        RENDER_API void CopyFrom(std::span<byte> _data, TextureView _dst);

        RENDER_API void TransitionTexture(TextureView _tex, ETextureStateFlags _dst_state, EPassType _pass);
        RENDER_API void TransitionBuffer(BufferView _buffer, EBufferRuntimeUsageFlags _dst_state, EPassType _pass);
        RENDER_API void AddCallback(std::function<void()>&& _callback);

        RENDER_API CmdSubmit Submit();

    private:
        friend DrawDispatcher;
        friend ComputeDispatcher;
        friend class CommandQueue;
        void                         SetRenderCmds(PipelineHandle& _handle, RenderPassInfo&&, Array<MeshDrawData>&&);
        void                         SubmitArgs(ShaderPipeline&, Arguments&&);
        void                         SubmitConstants(ShaderPipeline&, Array<uint>&&);
        Array<UniquePtr<Command>>    commands;
        Array<std::function<void()>> callbacks;
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
}// namespace Moer::Render
#endif