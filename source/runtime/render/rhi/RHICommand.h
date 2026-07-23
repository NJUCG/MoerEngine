#ifndef MOER_ENGINE_RHI_COMMAND_H
#define MOER_ENGINE_RHI_COMMAND_H

#include "RHI.h"
#include "RHICommandDrawData.h"
#include "RHIIO.h"
#include "RenderAPI.h"
#include "math/Base.h"
#include "misc/STL.h"
#include "misc/Traits.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "rhi/RHISubmissionTopology.h"
#include "shader/ShaderPipeline.h"
#include <array>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <misc/STL.h>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

//a unified commandlist for all usage?
class RHICmdList {
public:
    struct Impl;
};

namespace Moer::Render {

struct ProfileSection {
    // Non-"Other" names must be unique across timestamped scopes and dispatches
    // within one profiled submission because each name owns one query pair.
    std::string_view name;
    explicit ProfileSection(const char* _name) : name(_name) {}
};

// RenderDoc/PIX marker colors are kept in one palette so frame captures use a
// stable visual language across renderers and passes.
namespace GpuMarkerPalette {
inline float4 Frame() noexcept {
    return {0.18f, 0.42f, 0.90f, 1.0f};
}
inline float4 Renderer() noexcept {
    return {0.20f, 0.65f, 0.90f, 1.0f};
}
inline float4 RenderGraph() noexcept {
    return {0.58f, 0.36f, 0.88f, 1.0f};
}
inline float4 Pass() noexcept {
    return {0.18f, 0.72f, 0.52f, 1.0f};
}
inline float4 Subpass() noexcept {
    return {0.42f, 0.78f, 0.34f, 1.0f};
}
inline float4 Ui() noexcept {
    return {0.95f, 0.52f, 0.18f, 1.0f};
}
inline float4 Transfer() noexcept {
    return {0.90f, 0.72f, 0.18f, 1.0f};
}
inline float4 Scope() noexcept {
    return {0.35f, 0.75f, 0.45f, 1.0f};
}
} // namespace GpuMarkerPalette

enum class EGpuMarkerMode : uint8_t {
    Label,
    Timestamp,
};

struct Command {
public:
    enum class EType : uint8_t {
        UploadBuffer,
        CopyBackBuffer,
        BufferToBuffer,
        BufferToTexture,
        TextureToBuffer,
        UploadTexture,
        TextureToTexture,
        CopyBackTexture,
        ShaderDispatch,
        BuildAccel,
        BuildTLAS,
        TraceRay,
        Barrier,
        QueueTransfer,
        SetDrawState,
        SetGeometryPassDrawState,
        MultiDraw,
        UpdateBindlessArray,
        ClearResource,
        Scope,
        Custom,
        Count
    };

    static constexpr std::array<std::string_view, static_cast<size_t>(EType::Count)> typenames = {
        "UploadBuffer",    "CopyBackBuffer",      "BufferToBuffer", "BufferToTexture",
        "TextureToBuffer", "UploadTexture",       "TextureToTexture", "CopyBackTexture",
        "ShaderDispatch",  "BuildAccel",          "BuildTLAS",      "TraceRay",
        "Barrier",         "QueueTransfer",       "SetDrawState",   "SetGeometryPassDrawState",
        "MultiDraw",       "UpdateBindlessArray", "ClearResource",  "Scope",
        "Custom"
    };

private:
    EType type;

public:
    explicit Command(EType _type) : type(_type), name(typenames[static_cast<size_t>(_type)]) {}
    explicit Command(EType _type, std::string_view _name) : type(_type), name(_name) {}
    virtual ~Command()                      = default;
    virtual EQueueType GetQueueType() const = 0;

public:
    EType Type() const {
        return type;
    }
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

struct IndirectDrawParam {
    BufferView                buffer;
    std::optional<BufferView> count_buffer;

    //when count_buffer is not null, count is the max count of draw calls
    uint count;
    uint stride;
};

using TDrawParam = std::variant<Array<SingleDrawParam>, IndirectDrawParam>;
struct MeshDrawData {
    // 注意！为了性能，MeshDrawData内没有记录任何关于vtx_views的顶点布局信息。
    // vtx_views的数量与顺序应该在外部代码进行维护！MeshDrawData没有任何保护措施！
    // 关于内部顺序：buffer顺序应当按照EVertexAttributes枚举值的顺序排列。例如，PositionBuffer应该在NormalBuffer之前。（最自然的顺序）
    Array<VertexBuffer>             vtx_views;
    std::variant<IndexBuffer, uint> idx_view = 0u;

    Array<SingleDrawParam>           draw_params;
    std::optional<IndirectDrawParam> indirect_draw_param;

public:
    MeshDrawData() = default;
    MeshDrawData(MeshDrawData&& _other) noexcept {
        vtx_views           = std::move(_other.vtx_views);
        idx_view            = std::move(_other.idx_view);
        draw_params         = std::move(_other.draw_params);
        indirect_draw_param = std::move(_other.indirect_draw_param);
    }
    MeshDrawData& operator=(MeshDrawData&& _other) noexcept {
        vtx_views           = std::move(_other.vtx_views);
        idx_view            = std::move(_other.idx_view);
        draw_params         = std::move(_other.draw_params);
        indirect_draw_param = std::move(_other.indirect_draw_param);
        return *this;
    }

    MeshDrawData(const MeshDrawData& _other) {
        vtx_views           = _other.vtx_views;
        idx_view            = _other.idx_view;
        draw_params         = _other.draw_params;
        indirect_draw_param = _other.indirect_draw_param;
    }

    MeshDrawData& operator=(const MeshDrawData& _other) {
        vtx_views           = _other.vtx_views;
        idx_view            = _other.idx_view;
        draw_params         = _other.draw_params;
        indirect_draw_param = _other.indirect_draw_param;
        return *this;
    }

    // For better performance
    MeshDrawData(Array<VertexBuffer> _vtx_views, IndexBuffer _index_buffer) :
        vtx_views(std::move(_vtx_views)),
        idx_view(std::move(_index_buffer)) {}

    MeshDrawData(std::span<VertexBuffer> _vtx_views, IndexBuffer _index_buffer) : idx_view(_index_buffer) {
        vtx_views.assign(_vtx_views.begin(), _vtx_views.end());
    }

    MeshDrawData(std::span<VertexBuffer> _vtx_views, uint _index_cnt) : idx_view(_index_cnt) {
        vtx_views.assign(_vtx_views.begin(), _vtx_views.end());
    }

    void EmplaceDrawIndexed(
        uint _first_index,
        uint _index_cnt,
        uint _first_vertex,
        uint _first_instance,
        uint _instance_cnt = 1
    ) {

        draw_params.emplace_back(
            SingleDrawParam{_index_cnt, _instance_cnt, _first_index, _first_vertex, _first_instance}
        );
    }

    void DrawIndirect(BufferView _buffer, uint _count, uint _stride) {
        indirect_draw_param = IndirectDrawParam{_buffer, std::nullopt, _count, _stride};
    }

    void DrawIndirect(BufferView _buffer, BufferView _count_buffer, uint _max_cnt, uint _stride) {
        indirect_draw_param = IndirectDrawParam{_buffer, _count_buffer, _max_cnt, _stride};
    }

    void EmplaceDraw(uint _vertex_cnt, uint _first_vertex, uint _first_instance, uint _instance_cnt = 1) {

        draw_params.emplace_back(
            SingleDrawParam{_vertex_cnt, _instance_cnt, 0, _first_vertex, _first_instance}
        );
    }
    void Reserve(uint _size) {
        draw_params.reserve(_size);
    }
};

//Mesh Shader
struct DispatchMeshData {
    std::variant<IndirectDrawParam, Vector3ui> draw_param;

    static DispatchMeshData Dispatch(Vector3ui _group_count) {
        return DispatchMeshData{_group_count};
    }

    static DispatchMeshData DispatchIndirect(BufferView _buffer, uint _count, uint _stride) {
        return DispatchMeshData{IndirectDrawParam{_buffer, std::nullopt, _count, _stride}};
    }

    static DispatchMeshData
    DispatchIndirectCount(BufferView _buffer, BufferView _count_buffer, uint _max_cnt, uint _stride) {
        return DispatchMeshData{IndirectDrawParam{_buffer, _count_buffer, _max_cnt, _stride}};
    }
};

// A normal TickProfiling submit owns a complete profiler frame.  Upper-level
// submission runtimes may instead split that frame across several immutable
// command sources while preserving one query-pool/reset/advance transaction.
enum class ERHIProfilingPhase : uint8 {
    Disabled,
    Complete,
    Begin,
    Continue,
    End,
};

struct CmdSubmit {
    Array<UniquePtr<Command>>        cmds;
    Array<std::function<void(void)>> callbacks;
    Array<std::function<void(void)>> success_callbacks;
    TCachedArgArray                  cached_args;
    Array<RHISubmitSegment>          segments;

    Array<WaitEvent>   wait_events;
    Array<SignalEvent> signal_events;
    bool               b_sync{false}; //force sync queue timeline
    // Kept as a compatibility bit for code which treats profiling as a submit
    // capability.  profiling_phase is authoritative for split frames.
    bool               b_tick_profiling{false};
    ERHIProfilingPhase profiling_phase{ERHIProfilingPhase::Disabled};
    bool               b_delete_resources{false};
    ERHITranslateExecutionClass translate_execution_class{
        ERHITranslateExecutionClass::Parallel
    };
    std::string        debug_label;
    float4             debug_label_color = GpuMarkerPalette::Frame();

    CmdSubmit&& Wait(Fence* _fence, uint64 _wait_value) {
        wait_events.emplace_back(uint64(_fence), _wait_value);
        return std::move(*this);
    }

    CmdSubmit&& Wait(WaitEvent _event) { // FIX waitevent.timelinehandle maybe not a fence?
        wait_events.emplace_back(_event);
        return std::move(*this);
    }

    CmdSubmit&& Signal(Fence* _fence, uint64 _signal_value) {
        signal_events.emplace_back(uint64(_fence), _signal_value);
        return std::move(*this);
    }

    CmdSubmit&& SetTranslateExecutionClass(ERHITranslateExecutionClass _execution_class) {
        translate_execution_class = _execution_class;
        return std::move(*this);
    }

    CmdSubmit&& DeleteResources() {
        b_delete_resources = true;
        return std::move(*this);
    }

    CmdSubmit&& TickProfiling() {
        b_tick_profiling = true;
        profiling_phase  = ERHIProfilingPhase::Complete;
        return std::move(*this);
    }

    CmdSubmit&& SetProfilingPhase(ERHIProfilingPhase _phase) {
        profiling_phase  = _phase;
        b_tick_profiling = _phase != ERHIProfilingPhase::Disabled;
        return std::move(*this);
    }

    ERHIProfilingPhase ProfilingPhase() const noexcept {
        // Preserve Complete semantics for any legacy producer which still
        // writes the compatibility bit directly.
        return profiling_phase != ERHIProfilingPhase::Disabled ?
                   profiling_phase :
               b_tick_profiling ? ERHIProfilingPhase::Complete :
                                  ERHIProfilingPhase::Disabled;
    }

    bool EmitsProfilingQueries() const noexcept {
        return ProfilingPhase() != ERHIProfilingPhase::Disabled;
    }

    bool BeginsProfilingFrame() const noexcept {
        const ERHIProfilingPhase phase = ProfilingPhase();
        return phase == ERHIProfilingPhase::Complete || phase == ERHIProfilingPhase::Begin;
    }

    bool EndsProfilingFrame() const noexcept {
        const ERHIProfilingPhase phase = ProfilingPhase();
        return phase == ERHIProfilingPhase::Complete || phase == ERHIProfilingPhase::End;
    }

    CmdSubmit&& DebugLabel(
        std::string_view _label,
        float4           _color = GpuMarkerPalette::Frame()
    ) {
        debug_label       = _label;
        debug_label_color = _color;
        return std::move(*this);
    }

    CmdSubmit(CmdSubmit&& _other) noexcept {
        cmds               = std::move(_other.cmds);
        callbacks          = std::move(_other.callbacks);
        success_callbacks  = std::move(_other.success_callbacks);
        wait_events        = std::move(_other.wait_events);
        signal_events      = std::move(_other.signal_events);
        cached_args        = std::move(_other.cached_args);
        segments           = std::move(_other.segments);
        b_sync             = _other.b_sync;
        b_tick_profiling   = _other.b_tick_profiling;
        profiling_phase    = _other.profiling_phase;
        b_delete_resources = _other.b_delete_resources;
        translate_execution_class = _other.translate_execution_class;
        debug_label        = std::move(_other.debug_label);
        debug_label_color  = _other.debug_label_color;
    }

    CmdSubmit& operator=(CmdSubmit&& _other) noexcept {
        cmds               = std::move(_other.cmds);
        callbacks          = std::move(_other.callbacks);
        success_callbacks  = std::move(_other.success_callbacks);
        wait_events        = std::move(_other.wait_events);
        signal_events      = std::move(_other.signal_events);
        cached_args        = std::move(_other.cached_args);
        segments           = std::move(_other.segments);
        b_sync             = _other.b_sync;
        b_tick_profiling   = _other.b_tick_profiling;
        profiling_phase    = _other.profiling_phase;
        b_delete_resources = _other.b_delete_resources;
        translate_execution_class = _other.translate_execution_class;
        debug_label        = std::move(_other.debug_label);
        debug_label_color  = _other.debug_label_color;
        return *this;
    }
    CmdSubmit(
        Array<UniquePtr<Command>>&&        _cmds,
        Array<std::function<void(void)>>&& _callbacks,
        Array<std::function<void(void)>>&& _success_callbacks,
        TCachedArgArray&&                  _cached_args
    ) :
        cmds(std::move(_cmds)),
        callbacks(std::move(_callbacks)),
        success_callbacks(std::move(_success_callbacks)),
        cached_args(std::move(_cached_args)) {}

    std::string ToString() const {
        std::string str = "Commands: [";
        for (auto& cmd : cmds) {
            str += cmd->name;
            str += ", ";
        }
        str += "]";
        return str;
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

struct DrawBatchElement {
    PipelineHandle                                             handle;
    std::variant<Array<MeshDrawData>, Array<DispatchMeshData>> mesh_dispatch_data;
    TShaderArgArray                                            args;

    DrawBatchElement(DrawBatchElement&& _other) noexcept {
        handle             = _other.handle;
        mesh_dispatch_data = std::move(_other.mesh_dispatch_data);
        args               = std::move(_other.args);
    }
    DrawBatchElement& operator=(DrawBatchElement&& _other) noexcept {
        handle             = _other.handle;
        mesh_dispatch_data = std::move(_other.mesh_dispatch_data);
        args               = std::move(_other.args);
        return *this;
    }
    DrawBatchElement(const DrawBatchElement& _other) noexcept {
        handle             = _other.handle;
        mesh_dispatch_data = _other.mesh_dispatch_data;
        args               = _other.args;
    }

    DrawBatchElement() {}

    void RegisterDrawDatas(Array<MeshDrawData>&& _mesh_data) {
        mesh_dispatch_data = std::move(_mesh_data);
    }

    void RegisterDrawData(MeshDrawData&& _mesh_data) {
        if (std::holds_alternative<Array<DispatchMeshData>>(mesh_dispatch_data)) {
            mesh_dispatch_data = Array<MeshDrawData>{};
        }
        std::get<Array<MeshDrawData>>(mesh_dispatch_data).emplace_back(std::move(_mesh_data));
    }

    void RegisterMeshDispatch(Array<DispatchMeshData>&& _dispatch_data) {
        mesh_dispatch_data = std::move(_dispatch_data);
    }

    void RegisterMeshDispatch(DispatchMeshData&& _dispatch_data) {
        if (std::holds_alternative<Array<MeshDrawData>>(mesh_dispatch_data)) {
            mesh_dispatch_data = Array<DispatchMeshData>{};
        }
        std::get<Array<DispatchMeshData>>(mesh_dispatch_data).emplace_back(std::move(_dispatch_data));
    }
};
//Contains Array of DrawCmdData with Same RenderTargets and depth
struct DrawBatch {
    Array<DrawBatchElement> draw_cmds;

    template<typename TPipeline, typename... Ts>
    DrawBatchElement& Emplace(PipelineHandle _handle, Ts&&... _args) {
        DrawBatchElement& cmd = draw_cmds.emplace_back();
        cmd.args              = std::move(TPipeline::SetArgs(std::forward<Ts>(_args)...));
        cmd.handle            = _handle;

        return cmd;
    }

    DrawBatchElement& Emplace(PipelineHandle _handle, ArrayArgReference _reference) {
        DrawBatchElement& cmd = draw_cmds.emplace_back();
        cmd.args              = _reference;
        cmd.handle            = _handle;

        return cmd;
    }

    DrawBatchElement& Emplace(ArrayArguments&& _args, PipelineHandle _handle) {
        DrawBatchElement& cmd = draw_cmds.emplace_back();
        cmd.args              = std::move(_args);
        cmd.handle            = _handle;

        return cmd;
    }
};

template<typename TInArg>
concept is_arg = std::is_same_v<std::remove_reference_t<TInArg>(), BufferView> ||
                 std::is_same_v<std::remove_reference_t<TInArg>(), TextureView> ||
                 std::is_same_v<std::remove_reference_t<TInArg>(), Buffer*> ||
                 std::is_same_v<std::remove_reference_t<TInArg>(), Texture*>;
class CommandList {

public:
    CommandList(const CommandList&)            = delete;
    CommandList& operator=(const CommandList&) = delete;

    CommandList(CommandList&&)            = default;
    CommandList& operator=(CommandList&&) = default;

public:
    struct RENDER_API DrawDispatcher {
        DrawDispatcher(RasterPipeline& _pso, CommandList& _cmd_list);

        DrawDispatcher(RasterPipeline& _pso, CommandList& _cmd_list, ArrayArguments&& _args);

        // Unnamed Draw with mesh data list and depth attachment
        template<typename... TRenderTarget>
        void Draw(
            Rect2D                _rect,
            Array<MeshDrawData>&& _mesh_data,
            DepthAttachment       _depth,
            TRenderTarget&&... _render_targets
        ) {
            RenderPassInfo pass_info({std::forward<TRenderTarget>(_render_targets)...}, _depth, _rect);
            cmd_list.SetRenderCmds(pso.handle, std::move(args), std::move(pass_info), std::move(_mesh_data));
        };

        // Unnamed Draw with mesh data and draw list and depth attachment
        template<typename... TRenderTarget>
        void Draw(
            Rect2D                   _rect,
            std::span<VertexBuffer>  _vtx,
            IndexBuffer              _idx,
            Array<SingleDrawParam>&& _mesh_data,
            DepthAttachment          _depth,
            TRenderTarget&&... _render_targets
        ) {

            Array<MeshDrawData> mesh_data;
            mesh_data.emplace_back(_vtx, _idx);
            mesh_data.back().draw_params = std::move(_mesh_data);

            Draw(_rect, std::move(mesh_data), _depth, std::forward<TRenderTarget>(_render_targets)...);
        };

        // Unnamed Draw with mesh data and draw list
        template<typename... TRenderTarget>
        void Draw(
            Rect2D                   _rect,
            std::span<VertexBuffer>  _vtx,
            IndexBuffer              _idx,
            Array<SingleDrawParam>&& _mesh_data,
            TRenderTarget&&... _render_targets
        ) {

            Array<MeshDrawData> mesh_data;
            mesh_data.emplace_back(_vtx, _idx);
            mesh_data.back().draw_params = std::move(_mesh_data);

            Draw(_rect, std::move(mesh_data), std::forward<TRenderTarget>(_render_targets)...);
        };

        // Unnamed Draw with draw list and depth attachment
        template<typename... TRenderTarget>
        void Draw(
            Rect2D                   _rect,
            std::span<VertexBuffer>  _vtx,
            uint                     _vtx_cnt,
            Array<SingleDrawParam>&& _mesh_data,
            DepthAttachment          _depth,
            TRenderTarget&&... _render_targets
        ) {
            RenderPassInfo      pass_info({std::forward<TRenderTarget>(_render_targets)...}, _depth, _rect);
            Array<MeshDrawData> mesh_data;
            mesh_data.emplace_back(_vtx, _vtx_cnt);
            mesh_data.back().draw_params = std::move(_mesh_data);

            cmd_list.SetRenderCmds(pso.handle, std::move(args), std::move(pass_info), std::move(mesh_data));
        };

        // Unnamed Draw with mesh data list
        template<typename... TRenderTarget>
        void Draw(Rect2D _rect, Array<MeshDrawData>&& _mesh_data, TRenderTarget&&... _render_targets) {
            Draw(
                _rect,
                std::move(_mesh_data),
                DepthAttachment{},
                std::forward<TRenderTarget>(_render_targets)...
            );
        };

        // Named Draw with mesh data list and depth attachment
        template<typename... TRenderTarget>
        void Draw(
            std::string_view      _name,
            Rect2D                _rect,
            Array<MeshDrawData>&& _mesh_data,
            DepthAttachment       _depth,
            TRenderTarget&&... _render_targets
        ) {
            RenderPassInfo pass_info({std::forward<TRenderTarget>(_render_targets)...}, _depth, _rect);
            cmd_list.SetRenderCmds(
                pso.handle, std::move(args), std::move(pass_info), std::move(_mesh_data), _name
            );
        };

        // Named Draw with mesh data and draw list and depth attachment
        template<typename... TRenderTarget>
        void Draw(
            std::string_view         _name,
            Rect2D                   _rect,
            std::span<VertexBuffer>  _vtx,
            IndexBuffer              _idx,
            Array<SingleDrawParam>&& _mesh_data,
            DepthAttachment          _depth,
            TRenderTarget&&... _render_targets
        ) {

            Array<MeshDrawData> mesh_data;
            mesh_data.emplace_back(_vtx, _idx);
            mesh_data.back().draw_params = std::move(_mesh_data);

            Draw(_name, _rect, std::move(mesh_data), _depth, std::forward<TRenderTarget>(_render_targets)...);
        };

        // Named Draw with mesh data and draw list
        template<typename... TRenderTarget>
        void Draw(
            std::string_view         _name,
            Rect2D                   _rect,
            std::span<VertexBuffer>  _vtx,
            IndexBuffer              _idx,
            Array<SingleDrawParam>&& _mesh_data,
            TRenderTarget&&... _render_targets
        ) {

            Array<MeshDrawData> mesh_data;
            mesh_data.emplace_back(_vtx, _idx);
            mesh_data.back().draw_params = std::move(_mesh_data);

            Draw(_name, _rect, std::move(mesh_data), std::forward<TRenderTarget>(_render_targets)...);
        };

        // Named Draw with draw list and depth attachment
        template<typename... TRenderTarget>
        void Draw(
            std::string_view         _name,
            Rect2D                   _rect,
            Array<SingleDrawParam>&& _mesh_data,
            DepthAttachment          _depth,
            TRenderTarget&&... _render_targets
        ) {
            RenderPassInfo      pass_info({std::forward<TRenderTarget>(_render_targets)...}, _depth, _rect);
            Array<MeshDrawData> mesh_data;
            mesh_data.emplace_back();
            mesh_data.back().draw_params = std::move(_mesh_data);

            cmd_list.SetRenderCmds(
                pso.handle, std::move(args), std::move(pass_info), std::move(mesh_data), _name
            );
        };

        // Named Draw with draw list
        template<typename... TRenderTarget>
        void Draw(
            std::string_view         _name,
            Rect2D                   _rect,
            Array<SingleDrawParam>&& _mesh_data,
            TRenderTarget&&... _render_targets
        ) {
            RenderPassInfo pass_info(
                {std::forward<TRenderTarget>(_render_targets)...}, DepthAttachment{}, _rect
            );
            Array<MeshDrawData> mesh_data;
            mesh_data.emplace_back();
            mesh_data.back().draw_params = std::move(_mesh_data);

            cmd_list.SetRenderCmds(
                pso.handle, std::move(args), std::move(pass_info), std::move(mesh_data), _name
            );
        };

        // Named Draw with mesh data and draw list and depth attachment
        template<typename... TRenderTarget>
        void Draw(
            std::string_view         _name,
            Rect2D                   _rect,
            std::span<VertexBuffer>  _vtx,
            uint                     _vtx_cnt,
            Array<SingleDrawParam>&& _mesh_data,
            DepthAttachment          _depth,
            TRenderTarget&&... _render_targets
        ) {
            RenderPassInfo      pass_info({std::forward<TRenderTarget>(_render_targets)...}, _depth, _rect);
            Array<MeshDrawData> mesh_data;
            mesh_data.emplace_back(_vtx, _vtx_cnt);
            mesh_data.back().draw_params = std::move(_mesh_data);

            cmd_list.SetRenderCmds(
                pso.handle, std::move(args), std::move(pass_info), std::move(mesh_data), _name
            );
        };

        // Named Draw with mesh data and draw list
        template<typename... TRenderTarget>
        void Draw(
            std::string_view         _name,
            Rect2D                   _rect,
            std::span<VertexBuffer>  _vtx,
            uint                     _vtx_cnt,
            Array<SingleDrawParam>&& _mesh_data,
            TRenderTarget&&... _render_targets
        ) {
            RenderPassInfo pass_info(
                {std::forward<TRenderTarget>(_render_targets)...}, DepthAttachment{}, _rect
            );
            Array<MeshDrawData> mesh_data;
            mesh_data.emplace_back(_vtx, _vtx_cnt);
            mesh_data.back().draw_params = std::move(_mesh_data);

            cmd_list.SetRenderCmds(
                pso.handle, std::move(args), std::move(pass_info), std::move(mesh_data), _name
            );
        };

        // Named Draw with mesh data list
        template<typename... TRenderTarget>
        void Draw(
            std::string_view      _name,
            Rect2D                _rect,
            Array<MeshDrawData>&& _mesh_data,
            TRenderTarget&&... _render_targets
        ) {
            Draw(
                _name,
                _rect,
                std::move(_mesh_data),
                DepthAttachment{},
                std::forward<TRenderTarget>(_render_targets)...
            );
        };

        // Named Draw Indirect with mesh data and draw list
        template<typename... TRenderTarget>
        void DrawIndirect(
            std::string_view        _name,
            Rect2D                  _rect,
            std::span<VertexBuffer> _vtx,
            IndexBuffer             _idx,
            BufferView              _indirect_buffer,
            uint                    _count,
            uint                    _stride,
            TRenderTarget&&... _render_targets
        ) {
            RenderPassInfo pass_info(
                {std::forward<TRenderTarget>(_render_targets)...}, DepthAttachment{}, _rect
            );
            Array<MeshDrawData> mesh_data;
            mesh_data.emplace_back(_vtx, _idx);
            mesh_data.back().DrawIndirect(_indirect_buffer, _count, _stride);

            cmd_list.SetRenderCmds(
                pso.handle, std::move(args), std::move(pass_info), std::move(mesh_data), _name
            );
        };

        // Named Draw Indirect with mesh data and draw list and depth attachment
        template<typename... TRenderTarget>
        void DrawIndirect(
            std::string_view        _name,
            Rect2D                  _rect,
            std::span<VertexBuffer> _vtx,
            IndexBuffer             _idx,
            BufferView              _indirect_buffer,
            uint                    _count,
            uint                    _stride,
            DepthAttachment         _depth,
            TRenderTarget&&... _render_targets
        ) {
            RenderPassInfo      pass_info({std::forward<TRenderTarget>(_render_targets)...}, _depth, _rect);
            Array<MeshDrawData> mesh_data;
            mesh_data.emplace_back(_vtx, _idx);
            mesh_data.back().DrawIndirect(_indirect_buffer, _count, _stride);

            cmd_list.SetRenderCmds(
                pso.handle, std::move(args), std::move(pass_info), std::move(mesh_data), _name
            );
        };

        // Named Draw Indirect with mesh data and draw list and depth attachment and count buffer
        template<typename... TRenderTarget>
        void DrawIndirect(
            std::string_view        _name,
            Rect2D                  _rect,
            std::span<VertexBuffer> _vtx,
            IndexBuffer             _idx,
            BufferView              _indirect_buffer,
            BufferView              _count_buffer,
            uint                    _stride,
            uint                    _max_cnt,
            DepthAttachment         _depth,
            TRenderTarget&&... _render_targets
        ) {
            RenderPassInfo      pass_info({std::forward<TRenderTarget>(_render_targets)...}, _depth, _rect);
            Array<MeshDrawData> mesh_data;
            mesh_data.emplace_back(_vtx, _idx);
            mesh_data.back().DrawIndirect(_indirect_buffer, _count_buffer, _max_cnt, _stride);

            cmd_list.SetRenderCmds(
                pso.handle, std::move(args), std::move(pass_info), std::move(mesh_data), _name
            );
        };

        // Named Draw Indirect with mesh data and draw list and count buffer
        template<typename... TRenderTarget>
        void DrawIndirect(
            std::string_view        _name,
            Rect2D                  _rect,
            std::span<VertexBuffer> _vtx,
            IndexBuffer             _idx,
            BufferView              _indirect_buffer,
            BufferView              _count_buffer,
            uint                    _max_cnt,
            uint                    _stride,
            TRenderTarget&&... _render_targets
        ) {
            RenderPassInfo pass_info(
                {std::forward<TRenderTarget>(_render_targets)...}, DepthAttachment{}, _rect
            );
            Array<MeshDrawData> mesh_data;
            mesh_data.emplace_back(_vtx, _idx);
            mesh_data.back().DrawIndirect(_indirect_buffer, _count_buffer, _max_cnt, _stride);

            cmd_list.SetRenderCmds(
                pso.handle, std::move(args), std::move(pass_info), std::move(mesh_data), _name
            );
        };

        template<typename... TRenderTarget>
        void DrawIndirect(
            std::string_view        _name,
            Rect2D                  _rect,
            std::span<VertexBuffer> _vtx,
            uint                    _vtx_cnt,
            BufferView              _indirect_buffer,
            BufferView              _count_buffer,
            uint                    _max_cnt,
            uint                    _stride,
            DepthAttachment         _depth,
            TRenderTarget&&... _render_targets
        ) {
            RenderPassInfo      pass_info({std::forward<TRenderTarget>(_render_targets)...}, _depth, _rect);
            Array<MeshDrawData> mesh_data;
            mesh_data.emplace_back(_vtx, _vtx_cnt);
            mesh_data.back().DrawIndirect(_indirect_buffer, _count_buffer, _max_cnt, _stride);

            cmd_list.SetRenderCmds(
                pso.handle, std::move(args), std::move(pass_info), std::move(mesh_data), _name
            );
        };

        template<typename... TRenderTarget>
        void DrawIndirect(
            std::string_view        _name,
            Rect2D                  _rect,
            std::span<VertexBuffer> _vtx,
            uint                    _vtx_cnt,
            BufferView              _indirect_buffer,
            BufferView              _count_buffer,
            uint                    _max_cnt,
            uint                    _stride,
            TRenderTarget&&... _render_targets
        ) {
            RenderPassInfo pass_info(
                {std::forward<TRenderTarget>(_render_targets)...}, DepthAttachment{}, _rect
            );
            Array<MeshDrawData> mesh_data;
            mesh_data.emplace_back(_vtx, _vtx_cnt);
            mesh_data.back().DrawIndirect(_indirect_buffer, _count_buffer, _max_cnt, _stride);

            cmd_list.SetRenderCmds(
                pso.handle, std::move(args), std::move(pass_info), std::move(mesh_data), _name
            );
        };

        template<typename... TRenderTarget>
        void DrawIndirect(
            std::string_view        _name,
            Rect2D                  _rect,
            std::span<VertexBuffer> _vtx,
            uint                    _vtx_cnt,
            BufferView              _indirect_buffer,
            uint                    _count,
            uint                    _stride,
            TRenderTarget&&... _render_targets
        ) {
            RenderPassInfo pass_info(
                {std::forward<TRenderTarget>(_render_targets)...}, DepthAttachment{}, _rect
            );
            Array<MeshDrawData> mesh_data;
            mesh_data.emplace_back(_vtx, _vtx_cnt);
            mesh_data.back().DrawIndirect(_indirect_buffer, _count, _stride);

            cmd_list.SetRenderCmds(
                pso.handle, std::move(args), std::move(pass_info), std::move(mesh_data), _name
            );
        };

        template<typename... TRenderTarget>
        void DrawIndirect(
            std::string_view        _name,
            Rect2D                  _rect,
            std::span<VertexBuffer> _vtx,
            uint                    _vtx_cnt,
            BufferView              _indirect_buffer,
            uint                    _count,
            uint                    _stride,
            DepthAttachment         _depth,
            TRenderTarget&&... _render_targets
        ) {
            RenderPassInfo      pass_info({std::forward<TRenderTarget>(_render_targets)...}, _depth, _rect);
            Array<MeshDrawData> mesh_data;
            mesh_data.emplace_back(_vtx, _vtx_cnt);
            mesh_data.back().DrawIndirect(_indirect_buffer, _count, _stride);

            cmd_list.SetRenderCmds(
                pso.handle, std::move(args), std::move(pass_info), std::move(mesh_data), _name
            );
        };

        RasterPipeline& pso;
        CommandList&    cmd_list;
        ArrayArguments  args;

    private:
        TCachedArgArray args_cache;
    };

    struct RENDER_API MutiDrawDispatcher {
        template<typename... TRenderTarget>
        MutiDrawDispatcher(CommandList& _cmd_list, Rect2D _rect, TRenderTarget... _attachments) :
            cmd_list(_cmd_list) {
            pass_info =
                RenderPassInfo({std::forward<TRenderTarget>(_attachments)...}, DepthAttachment{}, _rect);
        }
        template<typename... TRenderTarget>
        MutiDrawDispatcher(
            CommandList&    _cmd_list,
            Rect2D          _rect,
            DepthAttachment _depth,
            TRenderTarget... _attachments
        ) :
            cmd_list(_cmd_list) {
            pass_info = RenderPassInfo({std::forward<TRenderTarget>(_attachments)...}, _depth, _rect);
        }
        CommandList& cmd_list;

        MutiDrawDispatcher& AcceptDrawBatch(DrawBatch&& _draw_batch) {
            draw_batch = std::move(_draw_batch);
            return *this;
        }
        void Dispatch() {
            cmd_list.SetMultiRenderCmds(std::move(pass_info), std::move(draw_batch), name);
        }

    private:
        RenderPassInfo   pass_info;
        DrawBatch        draw_batch;
        std::string_view name;
    };
    // struct RENDER_API DrawGeometryPassDispatcher {
    //     DrawGeometryPassDispatcher(CommandList& _cmd_list);
    //     DrawGeometryPassDispatcher(CommandList& _cmd_list, ArrayArguments&& _args);

    //     template<typename... TRenderTarget>
    //     void Draw(
    //         std::string_view                                             _name,
    //         Rect2D                                                       _rect,
    //         UnorderedMap<VertexAttributesBitmask, Array<MeshDrawData>>&& _mesh_data_array_map,
    //         DepthAttachment                                              _depth,
    //         TRenderTarget&&... _render_targets
    //         //
    //     ) {
    //         RenderPassInfo pass_info(
    //             {std::forward<TRenderTarget>(_render_targets)...},
    //             _depth,
    //             _rect);
    //         cmd_list.SetRenderGeometryPassCmds(std::move(args), std::move(pass_info), std::move(_mesh_data_array_map), _name);
    //     };

    //     CommandList&   cmd_list;
    //     ArrayArguments args;
    // };

    struct RENDER_API RaytracingDispatcher {
        CommandList&   cmd_list;
        ArrayArguments args;
        RTPipeline&    pso;
    };
    struct RENDER_API ComputeDispatcher {
        void Dispatch(
            Vector2ui        _group_count,
            std::string_view _name    = Command::typenames[(uint)Command::EType::ShaderDispatch],
            ProfileSection   _section = ProfileSection("Other")
        ) {
            Dispatch(uint3(_group_count, 1), _name, _section);
        }
        void Dispatch(
            Vector3ui        _group_count,
            std::string_view _name    = Command::typenames[(uint)Command::EType::ShaderDispatch],
            ProfileSection   _section = ProfileSection("Other")
        );
        void Dispatch(
            uint             _group_cnt,
            std::string_view _name    = Command::typenames[(uint)Command::EType::ShaderDispatch],
            ProfileSection   _section = ProfileSection("Other")
        ) {
            Dispatch(Vector3ui(_group_cnt, 1, 1), _name, _section);
        }
        void DispatchIndirect(
            BufferView,
            std::string_view _name    = Command::typenames[(uint)Command::EType::ShaderDispatch],
            ProfileSection   _section = ProfileSection("Other")
        );
        ComputeDispatcher(ComputePipeline& _pso, CommandList& _cmd_list, ArrayArguments&& _args);
        ComputeDispatcher(ComputePipeline& _pso, CommandList& _cmd_list);
        ComputeDispatcher(ComputePipeline& _pso, CommandList& _cmd_list, ArrayArgReference _args);
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
        TShaderArgArray  args;

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
    RENDER_API explicit CommandList(EQueueType _queue_type);
    class Impl;

    template<is_shader_pipeline TGfxPso, typename... TArgs>
    DrawDispatcher Gfx(TGfxPso& _pso, TArgs&&... _args) {
        if constexpr (sizeof...(TArgs) > 0) {
            ArrayArguments&& args = _pso.SetArgs(_args...);
            return DrawDispatcher(_pso, *this, std::move(args));
        }
        return DrawDispatcher(_pso, *this);
    }

    template<typename... TRenderTarget>
    MutiDrawDispatcher Gfx(std::string_view _name, Rect2D _rect, TRenderTarget&&... _attachments) {
        return MutiDrawDispatcher(*this, _rect, std::forward<TRenderTarget>(_attachments)...);
    }

    template<typename... TRenderTarget>
    MutiDrawDispatcher
    Gfx(std::string_view _name, Rect2D _rect, DepthAttachment _depth, TRenderTarget&&... _attachments) {
        return MutiDrawDispatcher(*this, _rect, _depth, std::forward<TRenderTarget>(_attachments)...);
    }

    // call this func like this: cmd_list.GfxGeometryPass<PSO_Definition>(args...).Draw(...);
    // template<is_shader_pipeline TGfxPso, typename... TArgs>
    // DrawGeometryPassDispatcher GfxGeometryPass(TArgs&&... _args) {
    //     if constexpr (sizeof...(TArgs) > 0) {
    //         ArrayArguments&& args = TGfxPso::SetArgs(_args...);
    //         return DrawGeometryPassDispatcher(*this, std::move(args));
    //     }
    //     return DrawGeometryPassDispatcher(*this);
    // }

    template<typename TComputePso, typename... TArgs>
        requires(TComputePso::InnerArgs::arg_size == sizeof...(TArgs))
    ComputeDispatcher Compute(TComputePso& _pso, TArgs&&... _args) {
        if constexpr (sizeof...(TArgs) > 0) {
            ArrayArguments&& args = _pso.SetArgs(_args...);
            return ComputeDispatcher(_pso, *this, std::move(args));
        }
        return ComputeDispatcher(_pso, *this);
    };

    template<typename TComputePso>
    ComputeDispatcher Compute(TComputePso& _pso, ArrayArgReference _arg_ref) {
        return ComputeDispatcher(_pso, *this, _arg_ref);
    };

    template<typename TRTPso, typename... TArgs>
    void RT(TRTPso& _pso, TArgs&&... _args) {
        // ArrayArguments&& args = std::move(_pso.SetArgs(_args...));
        // commands.push_back(MakeUnique<ShaderDispatchCmd>(_pso, std::move(args)));
    }

    RENDER_API void CopyFrom(
        BufferView       _src,
        BufferView       _dst,
        std::string_view _name = Command::typenames[(uint)Command::EType::BufferToBuffer]
    );
    RENDER_API void CopyFrom(
        TextureView      _src,
        TextureView      _dst,
        std::string_view _name = Command::typenames[(uint)Command::EType::TextureToTexture]
    );
    RENDER_API void CopyFrom(
        TextureView      _src,
        BufferView       _dst,
        std::string_view _name = Command::typenames[(uint)Command::EType::TextureToBuffer]
    );
    RENDER_API void CopyFrom(
        BufferView       _src,
        TextureView      _dst,
        std::string_view _name = Command::typenames[(uint)Command::EType::BufferToTexture]
    );
    RENDER_API void CopyFrom(
        std::span<byte>  _data,
        BufferView       _dst,
        std::string_view _name = Command::typenames[(uint)Command::EType::UploadBuffer]
    );
    RENDER_API void CopyFrom(
        std::span<byte>  _data,
        TextureView      _dst,
        std::string_view _name = Command::typenames[(uint)Command::EType::UploadTexture]
    );
    RENDER_API void CopyFrom(
        Array<byte>&&    _data,
        BufferView       _dst,
        std::string_view _name = Command::typenames[(uint)Command::EType::UploadBuffer]
    );
    RENDER_API void CopyFrom(
        Array<byte>&&    _data,
        TextureView      _dst,
        std::string_view _name = Command::typenames[(uint)Command::EType::UploadTexture]
    );
    RENDER_API void CopyFrom(
        BufferView       _src,
        std::span<byte>  _data,
        std::string_view _name = Command::typenames[(uint)Command::EType::CopyBackBuffer]
    );
    RENDER_API void CopyFrom(
        TextureView      _src,
        std::span<byte>  _data,
        std::string_view _name = Command::typenames[(uint)Command::EType::CopyBackTexture]
    );

    RENDER_API void UpdateBindlessArray(BindlessArrayRef _array);

    RENDER_API void ClearResource(BufferView _buffer, uint32_t _value);
    RENDER_API void ClearResource(TextureView _texture, float4 _color);
    RENDER_API void ClearResource(TextureView _texture, uint32_t _value);

    RENDER_API void PushScope(
        std::string_view _name,
        float4           _color = GpuMarkerPalette::Scope()
    );
    RENDER_API void PopScope();

    RENDER_API void PushScopeWithTimeScope(
        std::string_view _name,
        float4           _color = GpuMarkerPalette::Scope()
    );
    RENDER_API void PopScopeWithTimeScope();

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

    void TextureBarriers(
        EQueueType            _src_queue,
        EQueueType            _dst_queue,
        EPassType             _pass,
        Array<ReadTexture>&&  _read_tex,
        Array<WriteTexture>&& _write_tex
    ) {
        BeginBarriers(_read_tex.size(), _write_tex.size(), 0, 0, _src_queue, _dst_queue);
        for (auto& tex : _read_tex) {
            InnerBarrier(tex, _pass);
        }
        for (auto& tex : _write_tex) {
            InnerBarrier(tex, _pass);
        }
        EndBarriers();
    }

    void BufferBarriers(
        EQueueType           _src_queue,
        EQueueType           _dst_queue,
        EPassType            _pass,
        Array<ReadBuffer>&&  _read_buf,
        Array<WriteBuffer>&& _write_buf
    ) {
        BeginBarriers(0, 0, _read_buf.size(), _write_buf.size(), _src_queue, _dst_queue);
        for (auto& buf : _read_buf) {
            InnerBarrier(buf, _pass);
        }
        for (auto& buf : _write_buf) {
            InnerBarrier(buf, _pass);
        }
        EndBarriers();
    }

    void TextureBarriers(
        EQueueType           _src_queue,
        EQueueType           _dst_queue,
        EPassType            _pass,
        Array<ReadTexture>&& _read_tex
    ) {
        BeginBarriers(_read_tex.size(), 0, 0, 0, _src_queue, _dst_queue);
        for (auto& tex : _read_tex) {
            InnerBarrier(tex, _pass);
        }
        EndBarriers();
    }

    void TextureBarriers(
        EQueueType            _src_queue,
        EQueueType            _dst_queue,
        EPassType             _pass,
        Array<WriteTexture>&& _write_tex
    ) {
        BeginBarriers(0, _write_tex.size(), 0, 0, _src_queue, _dst_queue);
        for (auto& tex : _write_tex) {
            InnerBarrier(tex, _pass);
        }
        EndBarriers();
    }

    RENDER_API void ImportResourcesFromQueue(
        EQueueType             _src_queue,
        Array<ImportTexture>&& _textures_to_import,
        Array<ImportBuffer>&&  _buffers_to_import
    );
    RENDER_API void ExportResourcesToQueue(
        EQueueType             _dst_queue,
        Array<ExportTexture>&& _textures_to_export,
        Array<ExportBuffer>&&  _buffers_to_export
    );

#pragma region[ raytracing ]

    RENDER_API void BuildAccelerationStructures(Array<AccelerationStructureBuildParam>&& _params);

    RENDER_API void UpdateRaytracingScene(RaytracingSceneRef _scene);

#pragma endregion

#pragma region[ custom commands ]

    RENDER_API void AddCustomCommand(
        UniquePtr<Command>&& _cmd,
        std::string_view     _name = Command::typenames[(uint)Command::EType::Custom]
    );

#pragma endregion

    RENDER_API void AddCallback(std::function<void()>&& _callback);
    RENDER_API void AddSuccessCallback(std::function<void()>&& _callback);

    RENDER_API ArrayArgReference RegisterArgs(ArrayArguments&& _args);

    // Terminal rejection-only ownership path. The producer must have stopped
    // mutating this CommandList before it is called. It destructively drops all
    // partial GPU commands, cached arguments, success callbacks and unclosed
    // scopes, returning only ordinary callbacks that must run for CPU/resource
    // cleanup. Unlike Submit(), this never creates a GPU-submittable payload.
    RENDER_API Array<std::function<void()>> DrainOrdinaryCallbacksForRejection();

    RENDER_API CmdSubmit Submit();

    RENDER_API bool IsEmpty() const;

    EQueueType GetQueueType() const noexcept {
        return queue_type;
    }

    CommandList& SetTranslateExecutionClass(
        ERHITranslateExecutionClass _execution_class
    ) noexcept {
        translate_execution_class = _execution_class;
        return *this;
    }

private:
    friend DrawDispatcher;
    friend ComputeDispatcher;
    friend class CommandQueue;
    RENDER_API void SetRenderCmds(
        PipelineHandle&  _handle,
        ArrayArguments&& _args,
        RenderPassInfo&&,
        Array<MeshDrawData>&&,
        std::optional<std::string_view> _name = std::nullopt
    );
    // void SubmitArgs(ShaderPipeline&, Arguments&&);
    // void SubmitConstants(ShaderPipeline&, Array<uint>&&);
    RENDER_API void SetMultiRenderCmds(RenderPassInfo&&, DrawBatch&&, std::string_view _name);
    // Specialized for Geometry Pass
    // RENDER_API void SetRenderGeometryPassCmds(
    //     ArrayArguments&&                                             _args,
    //     RenderPassInfo&&                                             _info,
    //     UnorderedMap<VertexAttributesBitmask, Array<MeshDrawData>>&& _mesh_data,
    //     std::string_view                                             _name);

    // Specialized for Geometry Pass
    RENDER_API void SetRenderGeometryPassCmds(
        ArrayArguments&&                                             _args,
        RenderPassInfo&&                                             _info,
        UnorderedMap<VertexAttributesBitmask, Array<MeshDrawData>>&& _mesh_data,
        std::string_view                                             _name
    );

    RENDER_API void SetRenderShadowDepthPassCmds(
        ArrayArguments&&                                             _args,
        RenderPassInfo&&                                             _info,
        UnorderedMap<VertexAttributesBitmask, Array<MeshDrawData>>&& _mesh_data,
        std::string_view                                             _name
    );

    RENDER_API void BeginBarriers(
        uint       _read_tex_cnt,
        uint       _write_tex_cnt,
        uint       _read_buf_cnt,
        uint       _write_buf_cnt,
        EQueueType _src_queue,
        EQueueType _dst_queue
    );
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
    Array<std::function<void()>> success_callbacks;
    TCachedArgArray              cached_args;
    EQueueType                   queue_type{EQueueType::Graphics};
    ERHITranslateExecutionClass  translate_execution_class{
        ERHITranslateExecutionClass::Parallel
    };
    struct ScopeState {
        std::string name;
        float4      color;
        bool        query_timestamp = false;
    };

    Stack<ScopeState>            scope_stack;
    UnorderedSet<std::string>    timestamp_scope_names;
};

// Non-copyable, non-movable RAII wrapper used by high-level render code. It guarantees that a
// visual/timestamp scope is closed on every return path and prevents a scope
// from leaking across CommandList::Submit(). Keeping the guard immobile also
// prevents a non-top scope from being moved and accidentally popping a nested
// marker.
class [[nodiscard]] ScopedGpuMarker {
public:
    ScopedGpuMarker(
        CommandList&  _cmd_list,
        std::string_view _name,
        float4        _color = GpuMarkerPalette::Scope(),
        EGpuMarkerMode _mode = EGpuMarkerMode::Label
    ) :
        cmd_list(&_cmd_list),
        mode(_mode) {
        if (mode == EGpuMarkerMode::Timestamp) {
            cmd_list->PushScopeWithTimeScope(_name, _color);
        } else {
            cmd_list->PushScope(_name, _color);
        }
    }

    ~ScopedGpuMarker() {
        Close();
    }

    ScopedGpuMarker(const ScopedGpuMarker&)            = delete;
    ScopedGpuMarker& operator=(const ScopedGpuMarker&) = delete;
    ScopedGpuMarker(ScopedGpuMarker&&)                 = delete;
    ScopedGpuMarker& operator=(ScopedGpuMarker&&)      = delete;

    void Close() noexcept {
        if (!cmd_list) {
            return;
        }
        if (mode == EGpuMarkerMode::Timestamp) {
            cmd_list->PopScopeWithTimeScope();
        } else {
            cmd_list->PopScope();
        }
        cmd_list = nullptr;
    }

private:
    CommandList*   cmd_list = nullptr;
    EGpuMarkerMode mode     = EGpuMarkerMode::Label;
};
class QueueCmd {};

struct ProfileResultEntry {
    std::string name;
    double      time;
};

struct ProfileData {
    Array<ProfileResultEntry> gpu_entries;
    Array<ProfileResultEntry> cpu_entries;
};

struct PresentReceiptResult {
    bool resolved              = false;
    bool submitted            = false;
    bool recreate_swapchain   = false;
};

class PresentReceipt {
public:
    void Resolve(bool _submitted, bool _recreate_swapchain = false) {
        {
            std::unique_lock<std::mutex> lock(mutex);
            if (resolved) {
                return;
            }
            result.resolved            = true;
            result.submitted           = _submitted;
            result.recreate_swapchain  = _recreate_swapchain;
            resolved                   = true;
        }
        cv.notify_all();
    }

    [[nodiscard]] PresentReceiptResult WaitForSubmission(
        std::chrono::milliseconds _timeout = std::chrono::seconds(10)
    ) {
        std::unique_lock<std::mutex> lock(mutex);
        if (!cv.wait_for(lock, _timeout, [this]() { return resolved; })) {
            return {};
        }
        return result;
    }

private:
    std::mutex               mutex;
    std::condition_variable  cv;
    PresentReceiptResult     result{};
    bool                     resolved{false};
};

using PresentReceiptRef = SharedPtr<PresentReceipt>;

class RENDER_API CommandQueue {
public:
    CommandQueue() {};
    CommandQueue(EQueueType _type, RenderDevice& _device);
    void                Test();
    virtual void        Wait(WaitEvent _event)                                = 0;
    virtual WaitEvent   Execute(CmdSubmit&& _submit)                          = 0;
    virtual void Present(
        SwapchainRef      _swapchain,
        TextureView       _target,
        PresentReceiptRef _receipt = {}
    ) = 0;
    virtual void        Sync()                                                = 0;
    virtual ProfileData GetProfilerEntry()                                    = 0;

    CommandQueue& operator=(CommandQueue& other) = delete;
    CommandQueue(const CommandQueue& other)      = delete;

    CommandQueue& operator=(CommandQueue&& other) = delete;
    CommandQueue(CommandQueue&& other)            = delete;
};

class RENDER_API CopyQueue {
public:
    CopyQueue() {};
    ~CopyQueue()                                           = default;
    virtual IOWaitEvt Execute(IOQueueSubmission&& _submit) = 0;
    virtual IOWaitEvt Execute(CmdSubmit&& _submit)         = 0;

    virtual void CopyFrom(BufferView _src, BufferView _dst)        = 0;
    virtual void CopyFrom(TextureView _src, TextureView _dst)      = 0;
    virtual void CopyFrom(TextureView _src, BufferView _dst)       = 0;
    virtual void CopyFrom(BufferView _src, TextureView _dst)       = 0;
    virtual void CopyFrom(std::span<byte> _data, BufferView _dst)  = 0;
    virtual void CopyFrom(std::span<byte> _data, TextureView _dst) = 0;

    virtual FenceRef GetFenceHandle()       = 0;
    virtual void     Sync(uint64 _timeline) = 0;

    CopyQueue& operator=(CopyQueue& other) = delete;
    CopyQueue(const CopyQueue& other)      = delete;

    CopyQueue& operator=(CopyQueue&& other) = delete;
    CopyQueue(CopyQueue&& other)            = delete;
};

class IOInterface;
using IOInterfaceRef = std::shared_ptr<IOInterface>;
struct IOService {
    static void Init();
    static void Dispose();

    static uint64_t     Execute(class IOCommandList& _cmd_list);
    static void         Sync(uint64_t _time_stamp);
    static IOInterface* CreateGPUService(CopyQueue* _copy_queue);
    static IOInterface* CreateCPUService();

    static void Destroy(IOInterface* _service);
    struct Impl;
};

class IOInterface {
public:
    virtual WaitEvent GetWaitEvent(uint64 _time_stamp)   = 0;
    virtual uint64    Execute(IOCommandList&& _cmd_list) = 0;
    virtual void      Sync(uint64_t _time_stamp)         = 0;
};
} // namespace Moer::Render
#endif
