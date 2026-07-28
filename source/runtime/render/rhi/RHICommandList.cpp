//
// Created by 17152 on 2023/9/21.
//
#include "PixelFormat.h"
#include "RHIImpl.h"
#include "Core.h"
#include "misc/STL.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "rhi/RHIResourceInitilizer.h"
#include "shader/ShaderPipeline.h"
#include "shader/ShaderResourceManager.h"
#include "string/StringConvert.h"
#include <atomic>
namespace Moer::Render {

namespace {
std::atomic<uint64_t> g_query_token_id{1};

bool IsValidPrimaryQueueType(EQueueType queue_type) {
    return queue_type == EQueueType::Graphics || queue_type == EQueueType::Compute;
}

bool IsFrameTickCommand(const Command* command) {
    if (command == nullptr || command->Type() != Command::EType::Custom) {
        return false;
    }
    const auto* custom_cmd = static_cast<const CustomCmd*>(command);
    return custom_cmd->CustomId() == CustomCmd::CustomCmdId::CUSTOM_FRAME_TICK;
}

bool HasSubmitSideEffects(
    const Array<std::function<void()>>& callbacks,
    const Array<WaitEvent>&             wait_events,
    const Array<SignalEvent>&           signal_events,
    const Array<QueryToken>&            query_tokens,
    const Array<GPUEvent>&              gpu_events,
    bool                                delete_resources
) {
    return !callbacks.empty() || !wait_events.empty() || !signal_events.empty() ||
           !query_tokens.empty() || !gpu_events.empty() || delete_resources;
}

Array<RHISubmitSegment> BuildSubmitSegments(
    const Array<UniquePtr<Command>>& commands,
    EQueueType                       root_queue,
    bool                             has_side_effects
) {
    Array<RHISubmitSegment> segments{};
    for (size_t cmd_index = 0; cmd_index < commands.size(); ++cmd_index) {
        const Command* command = commands[cmd_index].get();
        if (IsFrameTickCommand(command) && cmd_index + 1 != commands.size()) {
            LOG_ERROR(MOER_TEXT("CommandList::TickFrame must be the last top-level command in a CommandList"));
            assert(false && "TickFrame must be the last top-level command");
        }
    }

    if (!commands.empty()) {
        segments.emplace_back(RHISubmitSegment{
            .queue = root_queue,
            .begin = 0,
            .end = commands.size(),
        });
    }

    if (segments.empty() && has_side_effects) {
        segments.emplace_back(RHISubmitSegment{
            .queue = root_queue,
            .begin = 0,
            .end = 0,
        });
    }

    return segments;
}
}

void CommandQueue::Test() {
    ShaderManager manager = ShaderManager::Get();

    GfxPsoCreateInfo pso_info(
        RHIRasterizeInfo::Preset(),
        VertexStream(),
        {},
        RHIDepthStencilStateInfo::Preset(),
        PF_D32_SFLOAT_S8_UINT
    );

    GBufferLayout layout = manager.Raster().Vertex("").Pixel("").Build<GBufferLayout>(std::move(pso_info));
    CommandList   cmd_list;
    Array<MeshDrawData> draw_data;
    auto&&              draw_dispatcher = cmd_list.Gfx(layout);
    draw_dispatcher.Draw(Rect2D{}, std::move(draw_data), ColorAttachment{nullptr});
}

CommandList::CommandList() : CommandList(EQueueType::Graphics) {}

CommandList::CommandList(EQueueType _queue_type) : queue_type(_queue_type) {
    if (!IsValidPrimaryQueueType(_queue_type)) {
        LOG_ERROR(
            MOER_TEXT("CommandList only supports Graphics or Compute queue types, got={}"),
            static_cast<uint32>(_queue_type)
        );
        assert(false && "CommandList only supports Graphics or Compute queue types");
        queue_type = EQueueType::Graphics;
    }
}

// void CommandList::ArgSetter::SetBuffer(uint64 _index, BufferView _buffer) {
//     auto idx          = handle.GetBindingIdx(_index);
//     temp_args[_index] = _buffer;
// }

// void CommandList::ArgSetter::SetTexture(uint64 _index, TextureView _texture) {
//     auto idx          = handle.GetBindingIdx(_index);
//     temp_args[_index] = _texture;
// }

// void CommandList::ArgSetter::SetConstant(void* _data, uint _size) {
//     temp_constant.resize(_size);
//     std::memcpy(temp_constant.data(), _data, _size);
// }

// void CommandList::DrawDispatcher::SubmitArgsIfPossible() {
//     if (HasParams()) {
//         cmd_list.SubmitArgs(pso, arg_setter.StealArgs());
//     }
//     if (b_set_consts) {
//         cmd_list.SubmitConstants(pso, arg_setter.StealConstants());
//     }
//     b_set_params = false;
//     b_set_consts = false;
// }

CommandList::ComputeDispatcher::ComputeDispatcher(
    ComputePipeline& _pso,
    CommandList&     _cmd_list,
    ArrayArguments&& _args
) :
    cmd_list(_cmd_list),
    pso(_pso),
    args(std::move(_args)) {
    // cmd_list.commands.push_back(MakeUnique<SetParamsCmd>(_pso, std::move(_args)));
}
CommandList::ComputeDispatcher::ComputeDispatcher(ComputePipeline& _pso, CommandList& _cmd_list) :
    cmd_list(_cmd_list),
    pso(_pso),
    args(TEmptyShaderArg{}) {}

CommandList::ComputeDispatcher::ComputeDispatcher(
    ComputePipeline&  _pso,
    CommandList&      _cmd_list,
    ArrayArgReference _arg_ref
) :
    cmd_list(_cmd_list),
    pso(_pso),
    args(_arg_ref) {}

CommandList::DrawDispatcher::DrawDispatcher(
    RasterPipeline&  _pso,
    CommandList&     _cmd_list,
    ArrayArguments&& _args
) :
    cmd_list(_cmd_list),
    args(std::move(_args)),
    pso(_pso) {}

CommandList::DrawDispatcher::DrawDispatcher(RasterPipeline& _pso, CommandList& _cmd_list) :
    cmd_list(_cmd_list),
    pso(_pso),
    args({}) {}

void CommandList::ComputeDispatcher::Dispatch(
    uint3            _group_count,
    StringView _name,
    ProfileSection   _section
) {
    MOER_ASSERT(
        pso.handle.IsValid(),
        "Compute dispatch requires a valid PSO: {}",
        PlatformToUtf8(_name).Native()
    );
    cmd_list.commands.push_back(MakeUnique<DispatchCmd>(std::move(args), pso.handle, _group_count));
    cmd_list.commands.back()->name = _name;
}

void CommandList::ComputeDispatcher::DispatchIndirect(
    BufferView       _indirect,
    StringView _name,
    ProfileSection   _section
) {
    MOER_ASSERT(
        pso.handle.IsValid(),
        "Indirect compute dispatch requires a valid PSO: {}",
        PlatformToUtf8(_name).Native()
    );
    cmd_list.commands.push_back(MakeUnique<DispatchCmd>(std::move(args), pso.handle, _indirect));
    cmd_list.commands.back()->name = _name;
}

CmdSubmit CommandList::Submit() {
    if (b_buffer_overlap_active) {
        LOG_ERROR(MOER_TEXT("CommandList::Submit called while a BufferOverlap scope is still active"));
        assert(false && "CommandList::Submit called while a BufferOverlap scope is still active");
    }
    CmdSubmit submit(
        std::move(commands),
        std::move(callbacks),
        std::move(cached_args),
        std::move(query_tokens),
        std::move(gpu_events)
    );
    submit.wait_events        = std::move(submit_wait_events);
    submit.signal_events      = std::move(submit_signal_events);
    submit.wait_sync_points   = std::move(submit_wait_sync_points);
    submit.signal_sync_points = std::move(submit_signal_sync_points);
    submit.segments           = BuildSubmitSegments(
        submit.cmds,
        queue_type,
        HasSubmitSideEffects(
            submit.callbacks,
            submit.wait_events,
            submit.signal_events,
            submit.query_tokens,
            submit.gpu_events,
            submit_delete_resources
        )
    );
    // Scan for SetTrackAccess barriers and RHIFenceCmd
    for (const auto& cmd : submit.cmds) {
        if (cmd->Type() == Command::EType::Barrier) {
            const auto* barrier_cmd = static_cast<const BarrierCmd*>(cmd.get());
            if (barrier_cmd->ShouldUpdateTrackedState()) {
                b_non_parallel = true;
            }
        }
        if (cmd->Type() == Command::EType::RHIFence) {
            b_non_parallel = true;
            const auto* fence_cmd = static_cast<const RHIFenceCmd*>(cmd.get());
            fence_event = fence_cmd->FenceEvent();
        }
    }

    submit.record_complete_event = std::move(record_complete_event);
    submit.explicit_tracked_textures = std::move(explicit_tracked_textures);
    submit.explicit_tracked_buffers = std::move(explicit_tracked_buffers);
    submit.translate_execution_class = translate_execution_class;
    submit.translate_complete_event  = GraphEvent::CreateGraphEvent();
    submit.fence_event  = std::move(fence_event);
    submit.b_non_parallel = b_non_parallel;
    submit.b_delete_resources = submit_delete_resources;
    commands.clear();
    callbacks.clear();
    submit_wait_events.clear();
    submit_signal_events.clear();
    submit_wait_sync_points.clear();
    submit_signal_sync_points.clear();
    query_tokens.clear();
    gpu_events.clear();
    record_complete_event = nullptr;
    explicit_tracked_textures.clear();
    explicit_tracked_buffers.clear();
    translate_execution_class = ERHITranslateExecutionClass::Parallel;
    translate_complete_event = nullptr;
    fence_event = nullptr;
    b_non_parallel = false;
    submit_delete_resources = false;
    b_buffer_overlap_active = false;
    buffer_overlap_handle   = 0;
    return std::move(submit);
}

bool CommandList::IsEmpty() const {
    return commands.empty() && callbacks.empty() && cached_args.empty() &&
           submit_wait_events.empty() && submit_signal_events.empty() &&
           !submit_delete_resources;
}

void CommandList::CopyFrom(BufferView _src, BufferView _dst, StringView _name) {
    commands.push_back(
        MakeUnique<CopyBufferCmd>(
            reinterpret_cast<uint64>(_src.GetBuffer()),
            reinterpret_cast<uint64>(_dst.GetBuffer()),
            _src.byte_offset,
            _dst.byte_offset,
            _src.GetByteSize(),
            _name
        )
    );
}
void CommandList::CopyFrom(TextureView _src, TextureView _dst, StringView _name) {
    commands.push_back(
        MakeUnique<CopyTextureCmd>(
            _src.texture->GetFormat(),
            reinterpret_cast<uint64>(_src.texture), //reinterpret_cast<uint64
            reinterpret_cast<uint64>(_dst.texture),
            _src.mip_level,
            _dst.mip_level,
            _src.offset,
            _dst.offset,
            _src.extent,
            _name
        )
    );
}
void CommandList::CopyFrom(TextureView _src, BufferView _dst, StringView _name) {
    commands.push_back(
        MakeUnique<CopyTextureToBufferCmd>(
            _src.texture->GetFormat(),
            reinterpret_cast<uint64>(_src.texture),
            reinterpret_cast<uint64>(_dst.GetBuffer()),
            _src.offset,
            _dst.byte_offset,
            _src.extent,
            _src.mip_level,
            _src.array_layer,
            _name
        )
    );
}
void CommandList::CopyFrom(BufferView _src, TextureView _dst, StringView _name) {
    commands.push_back(
        MakeUnique<CopyBufferToTextureCmd>(
            _dst.texture->GetFormat(),
            reinterpret_cast<uint64>(_src.GetBuffer()),
            reinterpret_cast<uint64>(_dst.texture),
            _src.byte_offset,
            _dst.offset,
            _dst.extent,
            _dst.mip_level,
            _dst.array_layer,
            _name
        )
    );
}

// Be careful with the Lifetime of the data!
void CommandList::CopyFrom(std::span<byte> _data, TextureView _texture, StringView _name) {
    uint3 extent = uint3(
        std::max(uint(_texture.extent.x) >> _texture.mip_level, 1u),
        std::max(uint(_texture.extent.y) >> _texture.mip_level, 1u),
        std::max(uint(_texture.extent.z) >> _texture.mip_level, 1u)
    );
    commands.push_back(
        MakeUnique<UploadTextureCmd>(
            _texture.texture->GetFormat(),
            reinterpret_cast<uint64>(_texture.texture),
            _texture.mip_level,
            _texture.array_layer,
            _texture.offset,
            extent,
            _data.data(),
            _name
        )
    );
}

// Be careful with the Lifetime of the data!
void CommandList::CopyFrom(std::span<byte> _data, BufferView _buffer, StringView _name) {
    if (_data.size() == 0) {
        return;
    }
    commands.push_back(
        MakeUnique<UploadBufferCmd>(
            reinterpret_cast<uint64>(_buffer.GetBuffer()),
            _buffer.GetByteOffset(),
            _data.size_bytes(),
            _data.data(),
            _name
        )
    );
}

void CommandList::CopyFrom(BufferView _src, std::span<byte> _data, StringView _name) {
    (void)ReadbackCopy(_src, _data, _name);
}

void CommandList::CopyFrom(TextureView _src, std::span<byte> _data, StringView _name) {
    (void)ReadbackCopy(_src, _data, _name);
}

SyncPointRef CommandList::ReadbackCopy(
    BufferView       _src,
    std::span<byte>  _data,
    StringView _name
) {
    if (_src.GetBuffer() == nullptr || _data.empty()) {
        GraphEventRef empty_event = GraphEvent::CreateGraphEvent();
        empty_event->TryUnlockSubsequents(EThread::UNKNOWN_THREAD);
        return SyncPoint::Create(ESyncPointMode::GPUCPU, empty_event);
    }
    GraphEventRef completion_event = GraphEvent::CreateGraphEvent();
    commands.push_back(
        MakeUnique<CopyBackBufferCmd>(
            reinterpret_cast<uint64>(_src.GetBuffer()),
            _src.GetByteOffset(),
            _src.GetByteSize(),
            _data.data(),
            completion_event,
            _name
        )
    );
    return SyncPoint::Create(ESyncPointMode::GPUCPU, completion_event);
}

SyncPointRef CommandList::ReadbackCopy(
    TextureView      _src,
    std::span<byte>  _data,
    StringView _name
) {
    bool b_valid_size = _data.size() > 0 && _src.GetTexture();
    if (!b_valid_size) {
        GraphEventRef empty_event = GraphEvent::CreateGraphEvent();
        empty_event->TryUnlockSubsequents(EThread::UNKNOWN_THREAD);
        return SyncPoint::Create(ESyncPointMode::GPUCPU, empty_event);
    }
    uint mip_size = _src.GetTexture()->GetMipByteSize(_src.mip_level);
    b_valid_size &= mip_size <= _data.size();

    assert(b_valid_size && "Fatal: Copy back data size is less than texture mip size");
    GraphEventRef completion_event = GraphEvent::CreateGraphEvent();
    commands.push_back(
        MakeUnique<CopyBackTextureCmd>(
            reinterpret_cast<uint64>(_src.texture),
            _src.mip_level,
            _src.offset,
            _src.extent,
            std::span<byte>(_data.data(), mip_size),
            completion_event,
            _name
        )
    );
    return SyncPoint::Create(ESyncPointMode::GPUCPU, completion_event);
}

void CommandList::CopyFrom(Array<byte>&& _data, BufferView _dst, StringView _name) {
    if (_data.size() == 0) {
        return;
    }
    commands.push_back(
        MakeUnique<UploadBufferCmd>(
            reinterpret_cast<uint64>(_dst.GetBuffer()),
            _dst.GetByteOffset(),
            _data.size(),
            std::move(_data),
            _name
        )
    );
}

void CommandList::CopyFrom(Array<byte>&& _data, TextureView _dst, StringView _name) {
    if (_data.size() == 0) {
        return;
    }
    commands.push_back(
        MakeUnique<UploadTextureCmd>(
            _dst.texture->GetFormat(),
            reinterpret_cast<uint64>(_dst.texture),
            _dst.mip_level,
            _dst.array_layer,
            _dst.offset,
            _dst.extent,
            std::move(_data),
            _name
        )
    );
}

void CommandList::SetRenderCmds(
    PipelineHandle&                 _handle,
    ArrayArguments&&                _args,
    RenderPassInfo&&                _info,
    Array<MeshDrawData>&&           _mesh_data,
    std::optional<StringView> _name
) {
    if (_handle.IsValid() == false) {
        LOG_ERROR(
            MOER_TEXT("Attempt to dispatch a compute with invalid PSO. Please check if the PSO is created ")
            "successfully. PSO name: \"{}\"",
            _name.value_or(MOER_TEXT("Unnamed PSO"))
        );
    }

    commands.push_back(
        MakeUnique<SetDrawStateCmd>(_handle, std::move(_args), std::move(_info), std::move(_mesh_data))
    );
    if (_name.has_value()) {
        commands.back()->name = *_name;
    }
}

void CommandList::SetMultiRenderCmds(
    RenderPassInfo&& _pass_info,
    DrawBatch&&      _batch,
    StringView _name
) {
    commands.push_back(MakeUnique<MultiDrawCmd>(std::move(_batch), std::move(_pass_info), _name));
}

// Specialized for Geometry Pass
// void
// CommandList::SetRenderGeometryPassCmds(ArrayArguments&& _args, RenderPassInfo&& _info, UnorderedMap<VertexAttributesBitmask, Array<MeshDrawData>>&& _mesh_data_array_map, StringView _name
//                                        //
// ) {
//     commands.push_back(MakeUnique<SetGeometryPassDrawStateCmd>(std::move(_args), std::move(_info), std::move(_mesh_data_array_map), _name));
// }

void CommandList::UpdateBindlessArray(BindlessArrayRef _array) {
    assert(_array && "Bindless array is null");

    // One-shot deferred GPU initialization on first use
    if (_array->NeedsInit()) {
        _array->RecordInitCommands(*this);
    }

    commands.push_back(_array->CreateUpdateCommand());
}

void CommandList::ClearResource(BufferView _buffer, uint _value) {
    commands.emplace_back(MakeUnique<ClearResourceCmd>(_buffer, _value));
}

void CommandList::ClearResource(TextureView _texture, float4 _color) {
    commands.emplace_back(MakeUnique<ClearResourceCmd>(_texture, _color));
}

void CommandList::ClearResource(TextureView _texture, uint _value) {
    commands.emplace_back(MakeUnique<ClearResourceCmd>(_texture, _value));
}

void CommandList::PushScope(StringView _name) {
    commands.push_back(MakeUnique<ScopeCmd>(_name, true, false));
    scope_stack.push(_name);
}

QueryToken CommandList::CreateQueryToken(QueryKind _kind, StringView _name) {
    const uint64_t query_id = g_query_token_id.fetch_add(1, std::memory_order_relaxed);
    return QueryToken(query_id, _kind, String(_name), QueryFuture::Create());
}

QueryToken CommandList::BeginTimestampQuery(StringView _name) {
    QueryToken token = CreateQueryToken(QueryKind::Timestamp, _name);
    query_tokens.emplace_back(token);
    commands.push_back(MakeUnique<QueryCmd>(token, QueryCmd::EOp::BeginTimestamp, _name));
    return token;
}

void CommandList::EndTimestampQuery(const QueryToken& _token) {
    if (!_token.Valid()) {
        return;
    }
    commands.push_back(
        MakeUnique<QueryCmd>(
            _token,
            QueryCmd::EOp::EndTimestamp,
            _token.name.empty() ? Command::typenames[(uint)Command::EType::Query] : StringView(_token.name)
        )
    );
}

QueryToken CommandList::BeginOcclusionQuery(StringView _name) {
    QueryToken token = CreateQueryToken(QueryKind::Occlusion, _name);
    query_tokens.emplace_back(token);
    commands.push_back(MakeUnique<QueryCmd>(token, QueryCmd::EOp::BeginOcclusion, _name));
    return token;
}

void CommandList::EndOcclusionQuery(const QueryToken& _token) {
    if (!_token.Valid()) {
        return;
    }
    commands.push_back(
        MakeUnique<QueryCmd>(
            _token,
            QueryCmd::EOp::EndOcclusion,
            _token.name.empty() ? Command::typenames[(uint)Command::EType::Query] : StringView(_token.name)
        )
    );
}

void CommandList::PushScopeWithTimeScope(StringView _name) {
    PushScope(_name);
    QueryToken token = BeginTimestampQuery(_name);
    timed_scope_query_stack.push(token);
    gpu_events.push_back(GPUEvent{
        .type = GPUEvent::EType::BeginEvent,
        .name = String(_name),
        .query = token,
        .depth = ++event_depth,
        .timestamp_ns = 0
    });
#if MOER_TRACE_ENABLED && MOER_TRACE_GPU_ENABLED
    Utf8String trace_scope_name = PlatformToUtf8(_name);
    gpu_trace_scope_tokens.push(
        Moer::Trace::BeginSpan(
            Moer::Trace::SpanDesc{
                .name      = trace_scope_name,
                .category  = "GPU.CPU.Record",
                .track_type = Moer::Trace::TrackType::CPUThread
            }
        )
    );
#endif
}

void CommandList::PopScope() {
    assert(!scope_stack.empty() && "PopScope called with empty scope stack");
    if (scope_stack.empty()) {
        return;
    }
    commands.push_back(MakeUnique<ScopeCmd>(scope_stack.top(), false, false));
    scope_stack.pop();
}

void CommandList::PopScopeWithTimeScope() {
    assert(!scope_stack.empty() && "PopScopeWithTimeScope called with empty scope stack");
    if (scope_stack.empty()) {
        return;
    }
    PopScope();
    QueryToken token{};
    if (!timed_scope_query_stack.empty()) {
        token = timed_scope_query_stack.top();
        timed_scope_query_stack.pop();
        EndTimestampQuery(token);
    }
    if (token.Valid() && event_depth > 0) {
        gpu_events.push_back(GPUEvent{
            .type = GPUEvent::EType::EndEvent,
            .name = String(),
            .query = token,
            .depth = event_depth--,
            .timestamp_ns = 0
        });
    }
#if MOER_TRACE_ENABLED && MOER_TRACE_GPU_ENABLED
    if (!gpu_trace_scope_tokens.empty()) {
        Moer::Trace::SpanToken token = std::move(gpu_trace_scope_tokens.top());
        gpu_trace_scope_tokens.pop();
        Moer::Trace::EndSpan(std::move(token));
    }
#endif
}
#pragma region[ raytracing ]

void CommandList::BuildAccelerationStructures(Array<AccelerationStructureBuildParam>&& _geometries) {
    commands.emplace_back(MakeUnique<BuildAccelerationStructuresCmd>(std::move(_geometries)));
}

void CommandList::UpdateRaytracingScene(RaytracingSceneRef _scene) {
    UniquePtr<Command> command = _scene->UpdateScene();
    if (command) {
        commands.emplace_back(std::move(command));
    }
}

void CommandList::UpdateRaytracingScene(UniquePtr<Command>&& _prepared_update) {
    assert(_prepared_update && _prepared_update->Type() == Command::EType::BuildTLAS);
    commands.emplace_back(std::move(_prepared_update));
}

#pragma endregion
// void CommandList::SubmitArgs(ShaderPipeline& _pso, Arguments&& _args) {
//     commands.push_back(MakeUnique<SetParamsCmd>(_pso, std::move(_args)));
// }

// void CommandList::SubmitConstants(ShaderPipeline& _pso, Array<uint>&& _constants) {
//     commands.push_back(MakeUnique<SetConstantCmd>(_pso, std::move(_constants)));
// }

#pragma region[ custom commands ]

void CommandList::AddCustomCommand(UniquePtr<Command>&& _cmd, StringView _name) {
    commands.push_back(std::move(_cmd));
    commands.back()->name = _name;
}

CommandList& CommandList::TranslateFence(RHITranslateFence _fence) {
    if (!_fence.event) {
        LOG_ERROR(MOER_TEXT("CommandList::TranslateFence requires a valid GraphEventRef-backed fence"));
        assert(false && "TranslateFence requires a valid event");
        return *this;
    }

    commands.emplace_back(MakeUnique<TranslateFenceCmd>(std::move(_fence)));
    return *this;
}

CommandList& CommandList::LambdaCommand(
    std::function<void()>&& _callback,
    StringView        _name
) {
    if (!_callback) {
        LOG_ERROR(MOER_TEXT("CommandList::LambdaCommand requires a valid callback"));
        assert(false && "LambdaCommand requires a valid callback");
        return *this;
    }

    commands.emplace_back(MakeUnique<TranslateLambdaCmd>(std::move(_callback), _name));
    return *this;
}

#pragma endregion

void CommandList::AddCallback(std::function<void()>&& _callback) {
    callbacks.emplace_back(std::move(_callback));
}

CommandList& CommandList::Wait(Fence* _fence, uint64 _wait_value) {
    submit_wait_events.emplace_back(uint64(_fence), _wait_value);
    return *this;
}

CommandList& CommandList::Wait(WaitEvent _event) {
    submit_wait_events.emplace_back(_event);
    return *this;
}

CommandList& CommandList::Signal(Fence* _fence, uint64 _signal_value) {
    submit_signal_events.emplace_back(uint64(_fence), _signal_value);
    return *this;
}

CommandList& CommandList::Wait(SyncPointRef _sync_point) {
    if (_sync_point) {
        submit_wait_sync_points.emplace_back(std::move(_sync_point));
    }
    return *this;
}

CommandList& CommandList::Signal(SyncPointRef _sync_point) {
    if (_sync_point) {
        submit_signal_sync_points.emplace_back(std::move(_sync_point));
    }
    return *this;
}

CommandList& CommandList::SetTranslateExecutionClass(ERHITranslateExecutionClass _execution_class) {
    if (_execution_class == ERHITranslateExecutionClass::SerialControl) {
        b_non_parallel = true;
    }
    return *this;
}

CommandList& CommandList::RHIFence() {
    b_non_parallel = true;
    commands.emplace_back(MakeUnique<RHIFenceCmd>());
    return *this;
}

CommandList& CommandList::SetExplicitTrackedState(
    Array<TrackedTextureState>&& tracked_textures,
    Array<TrackedBufferState>&&  tracked_buffers
) {
    explicit_tracked_textures = std::move(tracked_textures);
    explicit_tracked_buffers = std::move(tracked_buffers);
    return *this;
}

CommandList& CommandList::SetRecordCompleteEvent(GraphEventRef _event) {
    record_complete_event = std::move(_event);
    return *this;
}

CommandList& CommandList::SetTrackedState(
    Array<TrackedTextureState>&& _textures,
    Array<TrackedBufferState>&&  _buffers
) {
    commands.emplace_back(MakeUnique<SetTrackedStateCmd>(std::move(_textures), std::move(_buffers)));
    return *this;
}

CommandList& CommandList::DeleteResources() {
    submit_delete_resources = true;
    return *this;
}

CommandList& CommandList::TickFrame() {
    if (!commands.empty() && IsFrameTickCommand(commands.back().get())) {
        LOG_ERROR(MOER_TEXT("CommandList::TickFrame can only be recorded once per CommandList"));
        assert(false && "TickFrame must not be recorded twice");
        return *this;
    }
    commands.emplace_back(MakeUnique<FrameTickCmd>());
    b_non_parallel = true;
    return *this;
}

namespace {
ERHIPipelineStageFlags GetShaderBarrierStage(EPassType pass_type) {
    switch (pass_type) {
        case EPassType::Graphics:
            return ERHIPipelineStageFlags::PS_ALL_GRAPHICS;
        case EPassType::Compute:
            return ERHIPipelineStageFlags::PS_COMPUTE_SHADER;
        case EPassType::Raytracing:
            return ERHIPipelineStageFlags::PS_RAY_TRACING_SHADER;
        case EPassType::Copy:
            return ERHIPipelineStageFlags::PS_TRANSFER;
        default:
            assert(false && "unsupported pass type");
            return ERHIPipelineStageFlags::PS_NONE;
    }
}
}

BarrierState MakeBarrierState(ETextureState state, EPassType pass_type) {
    switch (state) {
        case ETextureState::UNDEFINED:
            return {.stage = ERHIPipelineStageFlags::PS_NONE,
                    .access = ERHIAccessFlags::UNDEFINED};
        case ETextureState::TRANSFER_SRC:
            return {.stage = ERHIPipelineStageFlags::PS_TRANSFER,
                    .access = ERHIAccessFlags::TRANSFER_READ};
        case ETextureState::TRANSFER_DST:
            return {.stage = ERHIPipelineStageFlags::PS_TRANSFER,
                    .access = ERHIAccessFlags::TRANSFER_WRITE};
        case ETextureState::SHADER_RESOURCE:
            return {.stage = GetShaderBarrierStage(pass_type),
                    .access = ERHIAccessFlags::SHADER_READ | ERHIAccessFlags::SHADER_RESOURCE_VIEW};
        case ETextureState::SAMPLED:
            return {.stage = GetShaderBarrierStage(pass_type),
                    .access = ERHIAccessFlags::SHADER_READ | ERHIAccessFlags::SHADER_SAMPLED_READ};
        case ETextureState::RENDER_TARGET:
            return {.stage = ERHIPipelineStageFlags::PS_COLOR_ATTACHMENT_OUTPUT,
                    .access = ERHIAccessFlags::COLOR_ATTACHMENT_READ | ERHIAccessFlags::COLOR_ATTACHMENT_WRITE};
        case ETextureState::DEPTH_STENCIL_READ:
            return {.stage = ERHIPipelineStageFlags::PS_EARLY_FRAGMENT_TESTS |
                             ERHIPipelineStageFlags::PS_LATE_FRAGMENT_TESTS,
                    .access = ERHIAccessFlags::DEPTH_STENCIL_READ};
        case ETextureState::DEPTH_STENCIL_WRITE:
            return {.stage = ERHIPipelineStageFlags::PS_EARLY_FRAGMENT_TESTS |
                             ERHIPipelineStageFlags::PS_LATE_FRAGMENT_TESTS,
                    .access = ERHIAccessFlags::DEPTH_STENCIL_WRITE};
        case ETextureState::UNORDERED_ACCESS:
            return {.stage = GetShaderBarrierStage(pass_type),
                    .access = ERHIAccessFlags::SHADER_READ | ERHIAccessFlags::SHADER_WRITE |
                              ERHIAccessFlags::UNORDERED_ACCESS_VIEW};
        default:
            assert(false && "unsupported texture state");
            return {};
    }
}

BarrierState MakeBarrierState(EBufferState state, EPassType pass_type) {
    switch (state) {
        case EBufferState::UNDEFINED:
            return {.stage = ERHIPipelineStageFlags::PS_NONE,
                    .access = ERHIAccessFlags::UNDEFINED};
        case EBufferState::TRANSFER_SRC:
            return {.stage = ERHIPipelineStageFlags::PS_TRANSFER,
                    .access = ERHIAccessFlags::TRANSFER_READ};
        case EBufferState::TRANSFER_DST:
            return {.stage = ERHIPipelineStageFlags::PS_TRANSFER,
                    .access = ERHIAccessFlags::TRANSFER_WRITE};
        case EBufferState::VERTEX_BUFFER:
            return {.stage = ERHIPipelineStageFlags::PS_VERTEX_INPUT,
                    .access = ERHIAccessFlags::VERTEX_ATTRIBUTE_READ};
        case EBufferState::INDEX_BUFFER:
            return {.stage = ERHIPipelineStageFlags::PS_VERTEX_INPUT,
                    .access = ERHIAccessFlags::INDEX_READ};
        case EBufferState::INDIRECT_ARGUMENT:
            return {.stage = ERHIPipelineStageFlags::PS_DRAW_INDIRECT,
                    .access = ERHIAccessFlags::INDIRECT_COMMAND_READ};
        case EBufferState::SHADER_RESOURCE:
            return {.stage = GetShaderBarrierStage(pass_type),
                    .access = ERHIAccessFlags::SHADER_READ | ERHIAccessFlags::SHADER_RESOURCE_VIEW};
        case EBufferState::UNORDERED_ACCESS:
            return {.stage = GetShaderBarrierStage(pass_type),
                    .access = ERHIAccessFlags::SHADER_READ | ERHIAccessFlags::SHADER_WRITE |
                              ERHIAccessFlags::UNORDERED_ACCESS_VIEW};
        case EBufferState::ACCELERATION_STRUCTURE_BUILD_INPUT:
            return {.stage = ERHIPipelineStageFlags::PS_ACCELERATION_STRUCTURE_BUILD,
                    .access = ERHIAccessFlags::ACCELERATION_STRUCTURE_READ_BIT |
                              ERHIAccessFlags::SHADER_READ};
        case EBufferState::ACCELERATION_STRUCTURE_READ:
            return {.stage = ERHIPipelineStageFlags::PS_RAY_TRACING_SHADER,
                    .access = ERHIAccessFlags::ACCELERATION_STRUCTURE_READ_BIT};
        case EBufferState::ACCELERATION_STRUCTURE_WRITE:
            return {.stage = ERHIPipelineStageFlags::PS_ACCELERATION_STRUCTURE_BUILD,
                    .access = ERHIAccessFlags::ACCELERATION_STRUCTURE_WRITE_BIT};
        default:
            assert(false && "unsupported buffer state");
            return {};
    }
}

std::optional<ETextureState> TryGetTrackedTextureState(const BarrierState& state) {
    if (state.access == ERHIAccessFlags::UNDEFINED) {
        return ETextureState::UNDEFINED;
    }
    if ((state.access & ERHIAccessFlags::TRANSFER_READ) != ERHIAccessFlags::UNDEFINED) {
        return ETextureState::TRANSFER_SRC;
    }
    if ((state.access & ERHIAccessFlags::TRANSFER_WRITE) != ERHIAccessFlags::UNDEFINED) {
        return ETextureState::TRANSFER_DST;
    }
    if ((state.access & ERHIAccessFlags::COLOR_ATTACHMENT_WRITE) != ERHIAccessFlags::UNDEFINED) {
        return ETextureState::RENDER_TARGET;
    }
    if ((state.access & ERHIAccessFlags::DEPTH_STENCIL_WRITE) != ERHIAccessFlags::UNDEFINED) {
        return ETextureState::DEPTH_STENCIL_WRITE;
    }
    if ((state.access & ERHIAccessFlags::DEPTH_STENCIL_READ) != ERHIAccessFlags::UNDEFINED) {
        return ETextureState::DEPTH_STENCIL_READ;
    }
    if ((state.access & ERHIAccessFlags::SHADER_SAMPLED_READ) != ERHIAccessFlags::UNDEFINED) {
        return ETextureState::SAMPLED;
    }
    if (BarrierStateWrites(state)) {
        return ETextureState::UNORDERED_ACCESS;
    }
    if ((state.access & ERHIAccessFlags::SHADER_READ) != ERHIAccessFlags::UNDEFINED ||
        (state.access & ERHIAccessFlags::SHADER_RESOURCE_VIEW) != ERHIAccessFlags::UNDEFINED) {
        return ETextureState::SHADER_RESOURCE;
    }
    return std::nullopt;
}

std::optional<EBufferState> TryGetTrackedBufferState(const BarrierState& state) {
    if (state.access == ERHIAccessFlags::UNDEFINED) {
        return EBufferState::UNDEFINED;
    }
    if ((state.access & ERHIAccessFlags::TRANSFER_WRITE) != ERHIAccessFlags::UNDEFINED) {
        return EBufferState::TRANSFER_DST;
    }
    if ((state.access & ERHIAccessFlags::TRANSFER_READ) != ERHIAccessFlags::UNDEFINED) {
        return EBufferState::TRANSFER_SRC;
    }
    if ((state.access & ERHIAccessFlags::VERTEX_ATTRIBUTE_READ) != ERHIAccessFlags::UNDEFINED) {
        return EBufferState::VERTEX_BUFFER;
    }
    if ((state.access & ERHIAccessFlags::INDEX_READ) != ERHIAccessFlags::UNDEFINED) {
        return EBufferState::INDEX_BUFFER;
    }
    if ((state.access & ERHIAccessFlags::INDIRECT_COMMAND_READ) != ERHIAccessFlags::UNDEFINED) {
        return EBufferState::INDIRECT_ARGUMENT;
    }
    if ((state.access & ERHIAccessFlags::ACCELERATION_STRUCTURE_WRITE_BIT) != ERHIAccessFlags::UNDEFINED) {
        return EBufferState::ACCELERATION_STRUCTURE_WRITE;
    }
    if ((state.access & ERHIAccessFlags::ACCELERATION_STRUCTURE_READ_BIT) != ERHIAccessFlags::UNDEFINED) {
        return EBufferState::ACCELERATION_STRUCTURE_READ;
    }
    if (BarrierStateWrites(state)) {
        return EBufferState::UNORDERED_ACCESS;
    }
    if ((state.access & ERHIAccessFlags::SHADER_READ) != ERHIAccessFlags::UNDEFINED ||
        (state.access & ERHIAccessFlags::SHADER_RESOURCE_VIEW) != ERHIAccessFlags::UNDEFINED) {
        return EBufferState::SHADER_RESOURCE;
    }
    return std::nullopt;
}

void CommandList::BeginBarriers(
    uint                    _barrier_cnt,
    EQueueType              _src_queue,
    EQueueType              _dst_queue,
    ETrackedStateUpdateMode _tracked_state
) {
    commands.push_back(
        MakeUnique<BarrierCmd>(
            _barrier_cnt,
            _src_queue,
            _dst_queue,
            _tracked_state
        )
    );
    current_barriers = commands.back().get();
}

void CommandList::InnerBarrier(const BarrierCreateInfo& barrier) {
    BarrierCmd* barrier_cmd = static_cast<BarrierCmd*>(current_barriers);
    barrier_cmd->AddBarrier(barrier);
}

void CommandList::InnerReadBindlessArray(BindlessArrayRef _array, EBufferState _state, EPassType _pass) {
    BarrierCmd* barrier = static_cast<BarrierCmd*>(current_barriers);
    barrier->ReadBindlessArray(_array, _state, _pass);
}

void CommandList::InnerWriteBindlessArray(BindlessArrayRef _array, EBufferState _state, EPassType _pass) {
    BarrierCmd* barrier = static_cast<BarrierCmd*>(current_barriers);
    barrier->WriteBindlessArray(_array, _state, _pass);
}

void CommandList::InnerReadRaytracingGeometry(
    RaytracingGeometryRef _geometry,
    EBufferState          _state,
    EPassType             _pass
) {
    BarrierCmd* barrier = static_cast<BarrierCmd*>(current_barriers);
    barrier->ReadAccelerationStructure(reinterpret_cast<uint64>(_geometry.Get()), _state, _pass);
}

void CommandList::InnerWriteRaytracingGeometry(
    RaytracingGeometryRef _geometry,
    EBufferState          _state,
    EPassType             _pass
) {
    BarrierCmd* barrier = static_cast<BarrierCmd*>(current_barriers);
    barrier->WriteAccelerationStructure(reinterpret_cast<uint64>(_geometry.Get()), _state, _pass);
}

void CommandList::InnerReadRaytracingTlas(RaytracingTlasRef _tlas, EBufferState _state, EPassType _pass) {
    BarrierCmd* barrier = static_cast<BarrierCmd*>(current_barriers);
    barrier->ReadAccelerationStructure(reinterpret_cast<uint64>(_tlas.Get()), _state, _pass);
}

void CommandList::InnerWriteRaytracingTlas(RaytracingTlasRef _tlas, EBufferState _state, EPassType _pass) {
    BarrierCmd* barrier = static_cast<BarrierCmd*>(current_barriers);
    barrier->WriteAccelerationStructure(reinterpret_cast<uint64>(_tlas.Get()), _state, _pass);
}

void CommandList::EndBarriers() {
    current_barriers = nullptr;
}

void CommandList::ImportResourcesFromQueue(
    EQueueType             _src_queue,
    Array<ImportTexture>&& _textures_to_import,
    Array<ImportBuffer>&&  _buffers_to_import
) {
    (void)_src_queue;
    (void)_textures_to_import;
    (void)_buffers_to_import;
    LOG_ERROR(MOER_TEXT("CommandList::ImportResourcesFromQueue has been removed. Use explicit barriers."));
    assert(false && "CommandList::ImportResourcesFromQueue has been removed");
}

void CommandList::ExportResourcesToQueue(
    EQueueType             _dst_queue,
    Array<ExportTexture>&& _textures_to_export,
    Array<ExportBuffer>&&  _buffers_to_export
) {
    (void)_dst_queue;
    (void)_textures_to_export;
    (void)_buffers_to_export;
    LOG_ERROR(MOER_TEXT("CommandList::ExportResourcesToQueue has been removed. Use explicit barriers."));
    assert(false && "CommandList::ExportResourcesToQueue has been removed");
}

ArrayArgReference CommandList::RegisterArgs(ArrayArguments&& _args) {
    cached_args.push_back(std::move(_args));
    return ArrayArgReference(cached_args.size() - 1);
}

void CommandList::PushGPUEvent(GPUEvent&& event) {
    gpu_events.push_back(std::move(event));
}

Array<GPUEvent> CommandList::StealGPUEvents() {
    Array<GPUEvent> result = std::move(gpu_events);
    gpu_events.clear();
    return result;
}

void CommandList::BeginBufferOverlap(BufferView _buffer) {
    if (_buffer.GetBuffer() == nullptr) {
        LOG_ERROR(MOER_TEXT("BeginBufferOverlap requires a valid buffer"));
        assert(false && "BeginBufferOverlap requires a valid buffer");
        return;
    }
    if (b_buffer_overlap_active) {
        LOG_ERROR(MOER_TEXT("Nested BeginBufferOverlap is not allowed"));
        assert(false && "Nested BeginBufferOverlap is not allowed");
        return;
    }
    b_buffer_overlap_active = true;
    buffer_overlap_handle   = reinterpret_cast<uint64>(_buffer.GetBuffer());
    commands.emplace_back(MakeUnique<BufferOverlapCmd>(buffer_overlap_handle, true));
}

void CommandList::EndBufferOverlap(BufferView _buffer) {
    if (!b_buffer_overlap_active) {
        LOG_ERROR(MOER_TEXT("EndBufferOverlap called without matching BeginBufferOverlap"));
        assert(false && "EndBufferOverlap called without matching BeginBufferOverlap");
        return;
    }
    const uint64 handle = reinterpret_cast<uint64>(_buffer.GetBuffer());
    if (handle != buffer_overlap_handle) {
        LOG_ERROR(MOER_TEXT("EndBufferOverlap buffer mismatch"));
        assert(false && "EndBufferOverlap buffer mismatch");
        return;
    }
    commands.emplace_back(MakeUnique<BufferOverlapCmd>(buffer_overlap_handle, false));
    b_buffer_overlap_active = false;
    buffer_overlap_handle   = 0;
}

} // namespace Moer::Render
