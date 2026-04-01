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
#include "shader/ShaderPipeline.h"
#include "trace/Trace.h"
#include <filesystem>
#include <functional>
#include <misc/STL.h>
#include <optional>
#include <span>
#include <string_view>
#include <condition_variable>
#include <mutex>
#include <memory>
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
    std::string_view name;
    explicit ProfileSection(const char* _name) : name(_name) {}
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
        Query,
        Custom,
        CopyScope
    };

    static constexpr std::string_view typenames[] = {
        "UploadBuffer",    "CopyBackBuffer",      "BufferToBuffer", "BufferToTexture",
        "TextureToBuffer", "CopyBackTexture",     "UploadTexture",  "TextureToTexture",
        "ShaderDispatch",  "BuildAccel",          "BuildTLAS",      "TraceRay",
        "Barrier",         "QueueTransfer",       "SetDrawState",   "SetGeometryPassDrawState",
        "MultiDraw",       "UpdateBindlessArray", "ClearResource",  "Scope", "Query",
        "Custom",          "CopyScope"
    };

private:
    EType type;

public:
    explicit Command(EType _type) : type(_type), name(typenames[uint(_type)]) {}
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

enum class QueryKind : uint8_t {
    Timestamp = 0,
    Occlusion = 1
};

enum class QueryStatus : uint8_t {
    Pending = 0,
    Ready   = 1,
    Error   = 2
};

struct TimestampQueryResult {
    uint64_t begin_tick{0};
    uint64_t end_tick{0};
    double   duration_ns{0.0};
};

struct OcclusionQueryResult {
    uint64_t sample_count{0};
    bool     visible{false};
};

struct QueryResult {
    QueryKind kind{QueryKind::Timestamp};
    QueryStatus status{QueryStatus::Pending};
    uint64_t query_id{0};
    std::string name{};
    std::variant<std::monostate, TimestampQueryResult, OcclusionQueryResult> payload{};
};

class QueryFuture {
    struct SharedState;

public:
    using Callback = std::function<void(const QueryResult&)>;

    QueryFuture() = default;

    static QueryFuture Create() {
        return QueryFuture(std::make_shared<SharedState>());
    }

    bool Valid() const {
        return static_cast<bool>(state_);
    }

    bool IsReady() const {
        if (!state_) {
            return false;
        }
        std::scoped_lock lock(state_->mtx);
        return state_->ready;
    }

    QueryStatus Status() const {
        if (!state_) {
            return QueryStatus::Error;
        }
        std::scoped_lock lock(state_->mtx);
        return state_->result.status;
    }

    void Wait() const {
        if (!state_) {
            return;
        }
        std::unique_lock lock(state_->mtx);
        state_->cv.wait(lock, [&]() {
            return state_->ready;
        });
    }

    QueryResult Get() const {
        Wait();
        if (!state_) {
            QueryResult result{};
            result.status = QueryStatus::Error;
            return result;
        }
        std::scoped_lock lock(state_->mtx);
        return state_->result;
    }

    void Then(Callback callback) const {
        if (!state_ || !callback) {
            return;
        }
        bool        ready = false;
        QueryResult result{};
        {
            std::scoped_lock lock(state_->mtx);
            if (state_->ready) {
                ready  = true;
                result = state_->result;
            } else {
                state_->callbacks.emplace_back(std::move(callback));
            }
        }
        if (ready) {
            callback(result);
        }
    }

private:
    struct SharedState {
        mutable std::mutex      mtx{};
        std::condition_variable cv{};
        bool                    ready{false};
        QueryResult             result{};
        Array<Callback>         callbacks{};
    };

    explicit QueryFuture(std::shared_ptr<SharedState> _state) : state_(std::move(_state)) {}

    void Resolve(QueryResult&& _result) const {
        if (!state_) {
            return;
        }
        Array<Callback> callbacks{};
        {
            std::scoped_lock lock(state_->mtx);
            if (state_->ready) {
                return;
            }
            state_->ready  = true;
            state_->result = std::move(_result);
            callbacks      = std::move(state_->callbacks);
        }
        state_->cv.notify_all();
        for (auto& callback : callbacks) {
            if (callback) {
                callback(state_->result);
            }
        }
    }

    std::shared_ptr<SharedState> state_{};

    friend struct QueryToken;
};

struct QueryToken {
    uint64_t   id{0};
    QueryKind  kind{QueryKind::Timestamp};
    std::string name{};

    QueryToken() = default;

    bool Valid() const {
        return id != 0 && future.Valid();
    }

    bool IsReady() const {
        return future.IsReady();
    }

    QueryFuture GetFuture() const {
        return future;
    }

    void Then(QueryFuture::Callback callback) const {
        future.Then(std::move(callback));
    }

    // Internal completion path used by backend query runtimes.
    void Resolve(QueryResult _result) const {
        future.Resolve(std::move(_result));
    }

private:
    QueryToken(uint64_t _id, QueryKind _kind, std::string _name, QueryFuture _future) :
        id(_id),
        kind(_kind),
        name(std::move(_name)),
        future(std::move(_future)) {}

    QueryFuture future{};

    friend class CommandList;
    friend class QueryRuntime;
    friend class VulkanQueryRuntime;
    friend struct ProfilerStorage;
};

struct GPUEvent {
    enum class EType {
        BeginCommandList,
        EndCommandList,
        BeginEvent,
        EndEvent,
        FrameBound
    };

    EType       type;
    std::string name;
    QueryToken  query;
    uint32      depth{0};
    uint64      cpu_time_ns{0};
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
struct CmdSubmit {
    Array<UniquePtr<Command>>        cmds;
    Array<std::function<void(void)>> callbacks;
    TCachedArgArray                  cached_args;

    Array<WaitEvent>   wait_events;
    Array<SignalEvent> signal_events;
    Array<QueryToken>  query_tokens;
    Array<GPUEvent>    gpu_events;
    bool               b_sync{false}; //force sync queue timeline
    bool               b_tick_profiling{false};
    bool               b_delete_resources{false};

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

    CmdSubmit&& DeleteResources() {
        b_delete_resources = true;
        return std::move(*this);
    }

    CmdSubmit&& TickProfiling() {
        b_tick_profiling = true;
        return std::move(*this);
    }

    CmdSubmit(CmdSubmit&& _other) noexcept {
        cmds               = std::move(_other.cmds);
        callbacks          = std::move(_other.callbacks);
        wait_events        = std::move(_other.wait_events);
        signal_events      = std::move(_other.signal_events);
        query_tokens       = std::move(_other.query_tokens);
        gpu_events         = std::move(_other.gpu_events);
        cached_args        = std::move(_other.cached_args);
        b_sync             = _other.b_sync;
        b_tick_profiling   = _other.b_tick_profiling;
        b_delete_resources = _other.b_delete_resources;
    }

    CmdSubmit& operator=(CmdSubmit&& _other) noexcept {
        cmds               = std::move(_other.cmds);
        callbacks          = std::move(_other.callbacks);
        wait_events        = std::move(_other.wait_events);
        signal_events      = std::move(_other.signal_events);
        query_tokens       = std::move(_other.query_tokens);
        gpu_events         = std::move(_other.gpu_events);
        cached_args        = std::move(_other.cached_args);
        b_sync             = _other.b_sync;
        b_tick_profiling   = _other.b_tick_profiling;
        b_delete_resources = _other.b_delete_resources;
        return *this;
    }
    CmdSubmit(
        Array<UniquePtr<Command>>&&        _cmds,
        Array<std::function<void(void)>>&& _callbacks,
        TCachedArgArray&&                  _cached_args,
        Array<QueryToken>&&                _query_tokens,
        Array<GPUEvent>&&                  _gpu_events = {}
    ) :
        cmds(std::move(_cmds)),
        callbacks(std::move(_callbacks)),
        cached_args(std::move(_cached_args)),
        query_tokens(std::move(_query_tokens)),
        gpu_events(std::move(_gpu_events)) {}

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

// Forward declaration
class CommandList;

/**
 * §2.3 CopyCommandScope
 *
 * RAII object returned by CommandList::BeginCopyScope().
 * Copy commands recorded through this scope are embedded in the parent Graphics/Compute
 * CommandList as a tagged section. The executor splits the command list at submit time:
 *   gfx section → copy queue section → gfx section
 * and auto-generates acquire/release barriers at scope boundaries.
 *
 * Constraints:
 * - Only Graphics / Compute CommandList can call BeginCopyScope()
 * - Only copy / upload / readback commands allowed inside a scope
 * - Multiple scopes per CommandList are allowed; nesting is not
 */
class RENDER_API CopyCommandScope {
public:
    ~CopyCommandScope();

    CopyCommandScope(CopyCommandScope&&) noexcept;
    CopyCommandScope& operator=(CopyCommandScope&&) = delete;
    CopyCommandScope(const CopyCommandScope&)        = delete;
    CopyCommandScope& operator=(const CopyCommandScope&) = delete;

    void CopyFrom(
        BufferView       _src,
        BufferView       _dst,
        std::string_view _name = Command::typenames[(uint)Command::EType::BufferToBuffer]
    );
    void CopyFrom(
        TextureView      _src,
        TextureView      _dst,
        std::string_view _name = Command::typenames[(uint)Command::EType::TextureToTexture]
    );
    void CopyFrom(
        TextureView      _src,
        BufferView       _dst,
        std::string_view _name = Command::typenames[(uint)Command::EType::TextureToBuffer]
    );
    void CopyFrom(
        BufferView       _src,
        TextureView      _dst,
        std::string_view _name = Command::typenames[(uint)Command::EType::BufferToTexture]
    );
    void CopyFrom(
        std::span<byte>  _data,
        BufferView       _dst,
        std::string_view _name = Command::typenames[(uint)Command::EType::UploadBuffer]
    );
    void CopyFrom(
        std::span<byte>  _data,
        TextureView      _dst,
        std::string_view _name = Command::typenames[(uint)Command::EType::UploadTexture]
    );
    void CopyFrom(
        Array<byte>&&    _data,
        BufferView       _dst,
        std::string_view _name = Command::typenames[(uint)Command::EType::UploadBuffer]
    );
    void CopyFrom(
        Array<byte>&&    _data,
        TextureView      _dst,
        std::string_view _name = Command::typenames[(uint)Command::EType::UploadTexture]
    );
    void CopyFrom(
        BufferView       _src,
        std::span<byte>  _data,
        std::string_view _name = Command::typenames[(uint)Command::EType::CopyBackBuffer]
    );
    void CopyFrom(
        TextureView      _src,
        std::span<byte>  _data,
        std::string_view _name = Command::typenames[(uint)Command::EType::CopyBackBuffer]
    );

private:
    explicit CopyCommandScope(CommandList& _cmd_list);
    void PushCopyCommand(UniquePtr<Command>&& _cmd);
    CommandList* cmd_list{nullptr};
    Array<UniquePtr<Command>> copy_commands{};

    friend class CommandList;
};

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

    EQueueType GetQueueType() const {
        return queue_type;
    }

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
        std::string_view _name = Command::typenames[(uint)Command::EType::CopyBackBuffer]
    );

    RENDER_API void UpdateBindlessArray(BindlessArrayRef _array);

    RENDER_API void ClearResource(BufferView _buffer, uint32_t _value);
    RENDER_API void ClearResource(TextureView _texture, float4 _color);
    RENDER_API void ClearResource(TextureView _texture, uint32_t _value);

    RENDER_API void PushScope(std::string_view _name);
    RENDER_API void PopScope();

    RENDER_API void PushScopeWithTimeScope(std::string_view _name);
    RENDER_API void PopScopeWithTimeScope();

    RENDER_API QueryToken BeginTimestampQuery(
        std::string_view _name = Command::typenames[(uint)Command::EType::Query]
    );
    RENDER_API void EndTimestampQuery(const QueryToken& _token);

    RENDER_API QueryToken BeginOcclusionQuery(
        std::string_view _name = Command::typenames[(uint)Command::EType::Query]
    );
    RENDER_API void EndOcclusionQuery(const QueryToken& _token);

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
    RENDER_API CommandList& Wait(Fence* _fence, uint64 _wait_value);
    RENDER_API CommandList& Wait(WaitEvent _event);
    RENDER_API CommandList& Signal(Fence* _fence, uint64 _signal_value);
    RENDER_API CommandList& DeleteResources();
    RENDER_API CommandList& TickProfiling();

    RENDER_API ArrayArgReference RegisterArgs(ArrayArguments&& _args);

    RENDER_API CmdSubmit Submit();

    RENDER_API bool IsEmpty() const;

    /**
     * §2.3 BeginCopyScope
     *
     * Creates a CopyCommandScope that embeds copy operations in this Graphics/Compute
     * CommandList as a tagged section. The executor splits the command list at submit
     * time and automatically generates acquire/release barriers at scope boundaries.
     * Unknown resources (first use inside CopyScope) skip the gfx→copy release barrier.
     *
     * Only callable on Graphics or Compute queue CommandLists.
     * Multiple scopes per CommandList are allowed; nesting is not.
     */
    RENDER_API CopyCommandScope BeginCopyScope();

private:
    friend DrawDispatcher;
    friend ComputeDispatcher;
    friend class CommandQueue;
    friend class GPUEventScope;
    friend class CopyCommandScope;
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

    QueryToken CreateQueryToken(QueryKind _kind, std::string_view _name);
    void       EnsureNoActiveCopyScope(std::string_view _api_name) const;
    void       FinalizeCopyScope(Array<UniquePtr<Command>>&& _commands);

    RENDER_API void PushGPUEvent(GPUEvent&& event);
    RENDER_API Array<GPUEvent> StealGPUEvents();

    Array<UniquePtr<Command>>    commands;
    Command*                     current_barriers{nullptr};
    Array<std::function<void()>> callbacks;
    Array<WaitEvent>             submit_wait_events;
    Array<SignalEvent>           submit_signal_events;
    TCachedArgArray              cached_args;
    Array<QueryToken>            query_tokens;
    bool                         submit_tick_profiling{false};
    bool                         submit_delete_resources{false};
    EQueueType                   queue_type{EQueueType::Graphics};
    Stack<std::string_view>      scope_stack;
    Stack<QueryToken>            timed_scope_query_stack;
#if MOER_TRACE_ENABLED && MOER_TRACE_GPU_ENABLED
    Stack<Moer::Trace::SpanToken> gpu_trace_scope_tokens;
#endif
    Array<GPUEvent>              gpu_events;
    uint32                       event_depth{0};
    bool                         b_copy_scope_active{false};
};

class RENDER_API GpuScopeSpan {
public:
    GpuScopeSpan(CommandList& _cmd_list, std::string_view _name) : cmd_list(&_cmd_list) {
        cmd_list->PushScopeWithTimeScope(_name);
    }
    ~GpuScopeSpan() {
        if (cmd_list) {
            cmd_list->PopScopeWithTimeScope();
        }
    }

    GpuScopeSpan(const GpuScopeSpan&)            = delete;
    GpuScopeSpan& operator=(const GpuScopeSpan&) = delete;

private:
    CommandList* cmd_list{nullptr};
};

class RENDER_API TimestampQuerySpan {
public:
    TimestampQuerySpan(CommandList& _cmd_list, std::string_view _name) :
        cmd_list(&_cmd_list),
        token(_cmd_list.BeginTimestampQuery(_name)) {}

    ~TimestampQuerySpan() {
        if (cmd_list && token.Valid()) {
            cmd_list->EndTimestampQuery(token);
        }
    }

    TimestampQuerySpan(const TimestampQuerySpan&)            = delete;
    TimestampQuerySpan& operator=(const TimestampQuerySpan&) = delete;

private:
    CommandList* cmd_list{nullptr};
    QueryToken   token{};
};

class RENDER_API OcclusionQuerySpan {
public:
    OcclusionQuerySpan(CommandList& _cmd_list, std::string_view _name) :
        cmd_list(&_cmd_list),
        token(_cmd_list.BeginOcclusionQuery(_name)) {}

    ~OcclusionQuerySpan() {
        if (cmd_list && token.Valid()) {
            cmd_list->EndOcclusionQuery(token);
        }
    }

    OcclusionQuerySpan(const OcclusionQuerySpan&)            = delete;
    OcclusionQuerySpan& operator=(const OcclusionQuerySpan&) = delete;

private:
    CommandList* cmd_list{nullptr};
    QueryToken   token{};
};

class RENDER_API GPUEventScope {
public:
    GPUEventScope(CommandList& _cmd_list, std::string_view _name) : cmd_list(&_cmd_list) {
        cmd_list->PushGPUEvent(GPUEvent{
            .type = GPUEvent::EType::BeginEvent,
            .name = std::string(_name),
            .query = cmd_list->BeginTimestampQuery(),
            .depth = ++cmd_list->event_depth,
            .cpu_time_ns = 0
        });
    }

    ~GPUEventScope() {
        if (cmd_list) {
            cmd_list->PushGPUEvent(GPUEvent{
                .type = GPUEvent::EType::EndEvent,
                .name = "",
                .query = cmd_list->BeginTimestampQuery(),
                .depth = cmd_list->event_depth--,
                .cpu_time_ns = 0
            });
        }
    }

    GPUEventScope(const GPUEventScope&)            = delete;
    GPUEventScope& operator=(const GPUEventScope&) = delete;

private:
    CommandList* cmd_list{nullptr};
};

#define GPU_PROFILE_EVENT_BEGIN(cmd_list, name) \
    (cmd_list).PushGPUEvent(GPUEvent{ \
        .type = GPUEvent::EType::BeginEvent, \
        .name = std::string(name), \
        .query = (cmd_list).BeginTimestampQuery(), \
        .depth = ++(cmd_list).event_depth, \
        .cpu_time_ns = 0 \
    })

#define GPU_PROFILE_EVENT_END(cmd_list) \
    (cmd_list).PushGPUEvent(GPUEvent{ \
        .type = GPUEvent::EType::EndEvent, \
        .name = "", \
        .query = (cmd_list).BeginTimestampQuery(), \
        .depth = (cmd_list).event_depth--, \
        .cpu_time_ns = 0 \
    })

#define GPU_PROFILE_EVENT_SCOPE(cmd_list, name) \
    GPUEventScope _gpu_event_scope(cmd_list, name)
class QueueCmd {};

struct RHISubmitCmdList {
    Array<CmdSubmit> submits;
    EQueueType       queue{EQueueType::Graphics};

    //TODO: remove this because preprocess do all useful in digest
    Array<TextureView> write_textures;

    //TODO: remove this because preprocess do all useful in digest
    RHISubmitCmdList& MarkWriteTexture(TextureView _texture) {
        if (_texture.texture) {
            write_textures.emplace_back(_texture);
        }
        return *this;
    }

    RHISubmitCmdList()                                          = default;
    RHISubmitCmdList(RHISubmitCmdList&&) noexcept              = default;
    RHISubmitCmdList& operator=(RHISubmitCmdList&&) noexcept   = default;
    RHISubmitCmdList(const RHISubmitCmdList&)                  = delete;
    RHISubmitCmdList& operator=(const RHISubmitCmdList&)       = delete;
};

struct RHIPresentOp {
    SwapchainRef swapchain;
    TextureView  target;
    EQueueType   queue{EQueueType::Graphics};
};

using RHIExecOp = std::variant<RHISubmitCmdList, RHIPresentOp>;

enum class ERHIExecSubmitFlags : uint8 {
    None     = 0,
    FlushGPU = 1 << 0,
    FrameEnd = 1 << 1,
};

ENUM_BIT_OP_IMPL(ERHIExecSubmitFlags, FLAG)

struct RHIPresentRequest {
    SwapchainRef swapchain;
    TextureView  source;
};

struct RHIExecSubmitOptions {
    bool flush_gpu{true};
    bool frame_end{false};
};

class RENDER_API RHIExecutor {
public:
    static RHIExecutor& Get();

    void Submit(
        Array<CommandList>&& command_lists,
        ERHIExecSubmitFlags  flags   = ERHIExecSubmitFlags::FlushGPU,
        RHIPresentRequest*   present = nullptr
    );
private:
    void Submit(
        Array<RHISubmitCmdList>&& submit_lists,
        RHIExecSubmitOptions      options = {}
    );
    void Submit(Array<RHIExecOp>&& ops, RHIExecSubmitOptions options = {});
    std::mutex  submit_mutex;
    Array<RHIExecOp> pending_ops{};
    bool        pending_frame_end{false};
};

struct ProfileResultEntry {
    std::string name;
    double      time;
};

struct ProfileData {
    Array<ProfileResultEntry> gpu_entries;
    Array<ProfileResultEntry> cpu_entries;
};
class RENDER_API CommandQueue {
public:
    CommandQueue() {};
    CommandQueue(EQueueType _type, RenderDevice& _device);
    void                Test();
    virtual void        Wait(WaitEvent _event)                                = 0;
    virtual WaitEvent   Execute(CmdSubmit&& _submit)                          = 0;
    virtual void        Present(SwapchainRef _swapchain, TextureView _target) = 0;
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

#if MOER_TRACE_ENABLED && MOER_TRACE_GPU_ENABLED
#define MOER_GPU_SCOPE_SPAN_VAR_JOIN_IMPL(a, b) a##b
#define MOER_GPU_SCOPE_SPAN_VAR_JOIN(a, b) MOER_GPU_SCOPE_SPAN_VAR_JOIN_IMPL(a, b)
#define TRACE_GPU_SCOPE_SPAN(cmd_list_ref, scope_name)                                               \
    ::Moer::Render::GpuScopeSpan MOER_GPU_SCOPE_SPAN_VAR_JOIN(_moer_gpu_scope_span_, __LINE__)(     \
        (cmd_list_ref),                                                                               \
        (scope_name)                                                                                  \
    )
#else
#define TRACE_GPU_SCOPE_SPAN(cmd_list_ref, scope_name) ((void)0)
#endif

#endif
