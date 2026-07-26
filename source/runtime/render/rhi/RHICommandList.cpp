//
// Created by 17152 on 2023/9/21.
//
#include "PixelFormat.h"
#include "RHIImpl.h"
#include "misc/STL.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "rhi/RHIResourceInitilizer.h"
#include "shader/ShaderPipeline.h"
#include "shader/ShaderResourceManager.h"
#include <atomic>
#include <stdexcept>
namespace Moer::Render {
namespace {

std::atomic<uint64> g_query_token_id{1};
std::atomic<uint64> g_query_owner_id{1};

uint64 NextNonZeroId(std::atomic<uint64>& _counter) noexcept {
    uint64 id = _counter.fetch_add(1, std::memory_order_relaxed);
    while (id == 0) {
        id = _counter.fetch_add(1, std::memory_order_relaxed);
    }
    return id;
}

void InvokeCallbacksNoexcept(Array<std::function<void()>>& _callbacks) noexcept {
    for (auto& callback : _callbacks) {
        if (!callback) {
            continue;
        }
        try {
            callback();
        } catch (...) {
            // An invalid query shape is already terminal. Cleanup callbacks
            // must not replace the structural error reported by Submit().
        }
    }
}

} // namespace

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

CommandList::CommandList(EQueueType _queue_type) :
    queue_type(_queue_type),
    query_owner_id(NextNonZeroId(g_query_owner_id)) {
    assert(
        _queue_type == EQueueType::Graphics || _queue_type == EQueueType::Compute ||
        _queue_type == EQueueType::Copy
    );
}

CommandList::~CommandList() {
    RejectPendingSignals();
    RejectPendingQueries("query command list was destroyed before submission");
}

CommandList::CommandList(CommandList&& _other) :
    CommandList(_other.queue_type) {
    *this = std::move(_other);
}

CommandList& CommandList::operator=(CommandList&& _other) {
    if (this == &_other) {
        return *this;
    }
    if (managed_recording_lease_count != 0 ||
        _other.managed_recording_lease_count != 0) {
        throw std::logic_error(
            "CommandList move is forbidden while graph-managed recording is active"
        );
    }

    // Allocate the replacement generation before changing either list. Once
    // ownership starts moving, every remaining operation below is noexcept.
    QueryCancellationDomain moved_from_query_domain{};
    RejectPendingSignals();
    QueryCancellationView replaced_query_cancellation =
        query_cancellation_domain.GetView();
    Array<QueryToken> replaced_query_tokens = std::move(query_tokens);
    const QueryPublishBatch replaced_query_batch =
        QueryBackendAccess::BeginPublishBatch();
    const bool replaced_cancellation_published =
        replaced_query_cancellation.PublishCancellation(
            "query command list was replaced before submission",
            replaced_query_batch
        );
    QueryBackendAccess::PublishErrorsIfPending(
        replaced_query_tokens,
        "query command list was replaced before submission",
        replaced_query_batch
    );
    commands                    = std::move(_other.commands);
    current_barriers            = _other.current_barriers;
    callbacks                   = std::move(_other.callbacks);
    success_callbacks           = std::move(_other.success_callbacks);
    query_tokens                = std::move(_other.query_tokens);
    active_query_stack          = std::move(_other.active_query_stack);
    query_cancellation_domain   = std::move(_other.query_cancellation_domain);
    signal_events               = std::move(_other.signal_events);
    cached_args                 = std::move(_other.cached_args);
    queue_type                  = _other.queue_type;
    translate_execution_class   = _other.translate_execution_class;
    resource_state_ownership    = _other.resource_state_ownership;
    seal_generation             = _other.seal_generation;
    scope_stack                 = std::move(_other.scope_stack);
    timestamp_scope_names       = std::move(_other.timestamp_scope_names);
    query_owner_id              = _other.query_owner_id;

    _other.current_barriers          = nullptr;
    _other.translate_execution_class =
        ERHITranslateExecutionClass::Parallel;
    _other.resource_state_ownership =
        ERHIResourceStateOwnership::BackendTracked;
    _other.seal_generation   = 0;
    _other.query_owner_id    = NextNonZeroId(g_query_owner_id);
    _other.query_cancellation_domain = std::move(moved_from_query_domain);
    _other.query_tokens.clear();
    _other.active_query_stack.clear();
    _other.signal_events.clear();

    // Install the complete incoming generation before releasing user Query
    // callbacks so re-entrant recording cannot be overwritten by this move.
    if (replaced_cancellation_published) {
        replaced_query_cancellation.NotifyCancellation(
            replaced_query_batch
        );
    } else {
        replaced_query_cancellation.NotifyPublishedCancellation();
    }
    QueryBackendAccess::NotifyTerminals(
        replaced_query_tokens, replaced_query_batch
    );
    return *this;
}

CommandList::ManagedRecordingLease::~ManagedRecordingLease() {
    Release();
}

CommandList::ManagedRecordingLease::ManagedRecordingLease(
    ManagedRecordingLease&& _other
) noexcept :
    command_list(std::exchange(_other.command_list, nullptr)) {}

CommandList::ManagedRecordingLease&
CommandList::ManagedRecordingLease::operator=(ManagedRecordingLease&& _other) noexcept {
    if (this != &_other) {
        Release();
        command_list = std::exchange(_other.command_list, nullptr);
    }
    return *this;
}

void CommandList::ManagedRecordingLease::Release() noexcept {
    if (command_list == nullptr) {
        return;
    }
    assert(command_list->managed_recording_lease_count > 0);
    --command_list->managed_recording_lease_count;
    command_list = nullptr;
}

CommandList::ManagedRecordingLease CommandList::AcquireManagedRecordingLease() {
    if (managed_recording_lease_count != 0) {
        throw std::logic_error("CommandList already has a managed recording lease");
    }
    ++managed_recording_lease_count;
    return ManagedRecordingLease(*this);
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
    std::string_view _name,
    ProfileSection   _section
) {
    if (pso.handle.IsValid() == false) {
        LOG_ERROR(
            "Attempt to dispatch a compute with invalid PSO. Please check if the PSO is created "
            "successfully. PSO name: \"{}\"",
            _name
        );
    }
    cmd_list.commands.push_back(MakeUnique<DispatchCmd>(std::move(args), pso.handle, _group_count, _section));
    cmd_list.commands.back()->name = _name;
}

void CommandList::ComputeDispatcher::DispatchIndirect(
    BufferView       _indirect,
    std::string_view _name,
    ProfileSection   _section
) {
    cmd_list.commands.push_back(MakeUnique<DispatchCmd>(std::move(args), pso.handle, _indirect, _section));
    cmd_list.commands.back()->name = _name;
}

CmdSubmit CommandList::Submit() {
    if (managed_recording_lease_count != 0) {
        throw std::logic_error(
            "CommandList::Submit is forbidden while graph-managed recording is active"
        );
    }
    if (!active_query_stack.empty()) {
        Array<std::function<void()>> cleanup_callbacks =
            DrainOrdinaryCallbacksForRejection();
        InvokeCallbacksNoexcept(cleanup_callbacks);
        throw std::logic_error(
            "CommandList::Submit rejected an unclosed timestamp query"
        );
    }
    // Construct the next recording generation before transferring callbacks,
    // signals, and query ownership into CmdSubmit. Allocation failure therefore
    // leaves the current generation intact and terminalizable.
    QueryCancellationDomain next_query_domain{};
    QueryCancellationView submitted_query_cancellation =
        query_cancellation_domain.GetView();
    ++seal_generation;
    if (!scope_stack.empty()) {
        assert(scope_stack.empty() && "CommandList::Submit called with unclosed GPU marker scopes");
        // Keep release builds/captures structurally valid even if a caller
        // violates the API contract. Debug builds fail above at the source.
        while (!scope_stack.empty()) {
            const ScopeState& scope = scope_stack.top();
            commands.push_back(
                MakeUnique<ScopeCmd>(scope.name, false, scope.query_timestamp, scope.color)
            );
            scope_stack.pop();
        }
    }
    const bool has_submit_side_effects =
        !callbacks.empty() || !success_callbacks.empty() ||
        !signal_events.empty();
    Array<RHISubmitSegment> submit_segments{};
    if (!commands.empty() || has_submit_side_effects) {
        // Allocate the segment before transferring signal/callback ownership.
        // If allocation fails, the CommandList remains intact and can still
        // be retried or terminally drained by the recording owner.
        submit_segments.emplace_back(RHISubmitSegment{
            .queue = queue_type,
            .begin = 0,
            .end   = commands.size(),
        });
    }

    CmdSubmit submit(
        std::move(commands),
        std::move(callbacks),
        std::move(success_callbacks),
        std::move(cached_args)
    );
    submit.query_tokens = std::move(query_tokens);
    submit.signal_events = std::move(signal_events);
    submit.segments      = std::move(submit_segments);
    submit.translate_execution_class = translate_execution_class;
    submit.resource_state_ownership   = resource_state_ownership;
    // Opaque shutdown publishes Query Error while deliberately retaining the
    // callback ticket until the producer reaches an ownership boundary. If
    // that boundary is Submit(), transfer the complete cancellation token set
    // and its original ticket into CmdSubmit instead of losing the owner when
    // this CommandList rotates to its next recording generation.
    Array<QueryToken> deferred_cancellation_tokens{};
    const QueryPublishBatch deferred_cancellation_batch =
        submitted_query_cancellation.TakePublishedCancellationBatch(
            deferred_cancellation_tokens
        );
    if (deferred_cancellation_batch.Valid()) {
        submit.query_tokens.swap(deferred_cancellation_tokens);
        submit.query_publish_batch = deferred_cancellation_batch;
    }
    commands.clear();
    callbacks.clear();
    success_callbacks.clear();
    query_tokens.clear();
    active_query_stack.clear();
    query_cancellation_domain = std::move(next_query_domain);
    signal_events.clear();
    timestamp_scope_names.clear();
    translate_execution_class = ERHITranslateExecutionClass::Parallel;
    resource_state_ownership  = ERHIResourceStateOwnership::BackendTracked;
    return std::move(submit);
}

Array<std::function<void()>> CommandList::DrainOrdinaryCallbacksForRejection() {
    if (managed_recording_lease_count != 0) {
        throw std::logic_error(
            "CommandList rejection draining is forbidden while graph-managed recording is active"
        );
    }
    // Do not move cleanup callbacks out until the replacement generation is
    // guaranteed to exist; rejection must retain its no-throw ownership tail.
    QueryCancellationDomain next_query_domain{};
    ++seal_generation;
    RejectPendingSignals();
    QueryCancellationView rejected_query_cancellation =
        query_cancellation_domain.GetView();
    Array<QueryToken> rejected_query_tokens = std::move(query_tokens);
    const QueryPublishBatch rejected_query_batch =
        QueryBackendAccess::BeginPublishBatch();
    const bool rejected_cancellation_published =
        rejected_query_cancellation.PublishCancellation(
            "query command list was rejected before completion",
            rejected_query_batch
        );
    QueryBackendAccess::PublishErrorsIfPending(
        rejected_query_tokens,
        "query command list was rejected before completion",
        rejected_query_batch
    );
    Array<std::function<void()>> cleanup_callbacks = std::move(callbacks);

    // This is deliberately not implemented in terms of Submit(). A failed
    // producer may leave a partially-built barrier or an unmatched marker;
    // neither is executable, and Submit() correctly asserts on that shape in
    // Debug builds. Destroying the command payload releases its strong refs
    // without manufacturing end-marker commands or submission segments.
    commands.clear();
    callbacks.clear();
    success_callbacks.clear();
    query_tokens.clear();
    active_query_stack.clear();
    query_cancellation_domain = std::move(next_query_domain);
    signal_events.clear();
    cached_args.clear();
    current_barriers = nullptr;
    while (!scope_stack.empty()) {
        scope_stack.pop();
    }
    timestamp_scope_names.clear();
    translate_execution_class = ERHITranslateExecutionClass::Parallel;
    resource_state_ownership  = ERHIResourceStateOwnership::BackendTracked;

    // The fresh generation is fully installed before Query callbacks can
    // re-enter this CommandList and append new work.
    if (rejected_cancellation_published) {
        rejected_query_cancellation.NotifyCancellation(
            rejected_query_batch
        );
    } else {
        rejected_query_cancellation.NotifyPublishedCancellation();
    }
    QueryBackendAccess::NotifyTerminals(
        rejected_query_tokens, rejected_query_batch
    );
    return cleanup_callbacks;
}

bool CommandList::IsEmpty() const {
    return commands.empty() && callbacks.empty() &&
           success_callbacks.empty() && signal_events.empty() &&
           cached_args.empty() && query_tokens.empty() &&
           active_query_stack.empty();
}

void CommandList::CopyFrom(BufferView _src, BufferView _dst, std::string_view _name) {
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
void CommandList::CopyFrom(TextureView _src, TextureView _dst, std::string_view _name) {
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
void CommandList::CopyFrom(TextureView _src, BufferView _dst, std::string_view _name) {
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
void CommandList::CopyFrom(BufferView _src, TextureView _dst, std::string_view _name) {
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

void CommandList::CopyFrom(std::span<byte> _data, TextureView _texture, std::string_view _name) {
    CopyFrom(std::span<const byte>(_data.data(), _data.size()), _texture, _name);
}

void CommandList::CopyFrom(
    std::span<const byte> _data,
    TextureView           _texture,
    std::string_view      _name
) {
    uint3 extent = uint3(
        std::max(uint(_texture.extent.x) >> _texture.mip_level, 1u),
        std::max(uint(_texture.extent.y) >> _texture.mip_level, 1u),
        std::max(uint(_texture.extent.z) >> _texture.mip_level, 1u)
    );
    Array<byte> owned_data(_data.begin(), _data.end());
    commands.push_back(
        MakeUnique<UploadTextureCmd>(
            _texture.texture->GetFormat(),
            reinterpret_cast<uint64>(_texture.texture),
            _texture.mip_level,
            _texture.array_layer,
            _texture.offset,
            extent,
            std::move(owned_data),
            _name
        )
    );
}

void CommandList::CopyFrom(std::span<byte> _data, BufferView _buffer, std::string_view _name) {
    CopyFrom(std::span<const byte>(_data.data(), _data.size()), _buffer, _name);
}

void CommandList::CopyFrom(
    std::span<const byte> _data,
    BufferView            _buffer,
    std::string_view      _name
) {
    if (_data.size() == 0) {
        return;
    }
    Array<byte> owned_data(_data.begin(), _data.end());
    commands.push_back(
        MakeUnique<UploadBufferCmd>(
            reinterpret_cast<uint64>(_buffer.GetBuffer()),
            _buffer.GetByteOffset(),
            _data.size_bytes(),
            std::move(owned_data),
            _name
        )
    );
}

void CommandList::CopyFrom(BufferView _src, std::span<byte> _data, std::string_view _name) {
    commands.push_back(
        MakeUnique<CopyBackBufferCmd>(
            reinterpret_cast<uint64>(_src.GetBuffer()),
            _src.GetByteOffset(),
            _src.GetByteSize(),
            _data.data(),
            _name
        )
    );
}

void CommandList::CopyFrom(TextureView _src, std::span<byte> _data, std::string_view _name) {
    bool b_valid_size = _data.size() > 0 && _src.GetTexture();
    if (!b_valid_size) {
        return;
    }
    uint mip_size = _src.GetTexture()->GetMipByteSize(_src.mip_level);
    b_valid_size &= mip_size <= _data.size();

    assert(b_valid_size && "Fatal: Copy back data size is less than texture mip size");
    commands.push_back(
        MakeUnique<CopyBackTextureCmd>(
            reinterpret_cast<uint64>(_src.texture),
            _src.mip_level,
            _src.offset,
            _src.extent,
            std::span<byte>(_data.data(), mip_size),
            _name
        )
    );
}

void CommandList::CopyFrom(Array<byte>&& _data, BufferView _dst, std::string_view _name) {
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

void CommandList::CopyFrom(Array<byte>&& _data, TextureView _dst, std::string_view _name) {
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
    std::optional<std::string_view> _name
) {
    if (_handle.IsValid() == false) {
        LOG_ERROR(
            "Attempt to dispatch a compute with invalid PSO. Please check if the PSO is created "
            "successfully. PSO name: \"{}\"",
            _name.value_or("Unnamed PSO")
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
    std::string_view _name
) {
    commands.push_back(MakeUnique<MultiDrawCmd>(std::move(_batch), std::move(_pass_info), _name));
}

// Specialized for Geometry Pass
// void
// CommandList::SetRenderGeometryPassCmds(ArrayArguments&& _args, RenderPassInfo&& _info, UnorderedMap<VertexAttributesBitmask, Array<MeshDrawData>>&& _mesh_data_array_map, std::string_view _name
//                                        //
// ) {
//     commands.push_back(MakeUnique<SetGeometryPassDrawStateCmd>(std::move(_args), std::move(_info), std::move(_mesh_data_array_map), _name));
// }

void CommandList::UpdateBindlessArray(BindlessArrayRef _array) {
    assert(_array && "Bindless array is null");

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

void CommandList::PushScope(std::string_view _name, float4 _color) {
    assert(!_name.empty() && "GPU marker scope name must not be empty");
    const std::string scope_name = _name.empty() ? "Unnamed GPU Scope" : std::string(_name);
    commands.push_back(MakeUnique<ScopeCmd>(scope_name, true, false, _color));
    scope_stack.emplace(ScopeState{scope_name, _color, false});
}

void CommandList::PushScopeWithTimeScope(std::string_view _name, float4 _color) {
    assert(!_name.empty() && "GPU timestamp scope name must not be empty");
    const std::string scope_name = _name.empty() ? "Unnamed GPU Timestamp Scope" : std::string(_name);
    const bool unique_timestamp_name = timestamp_scope_names.emplace(scope_name).second;
    assert(
        unique_timestamp_name &&
        "GPU timestamp scope names must be unique within a CommandList submission"
    );

    // In release builds, a duplicate remains useful as a visual marker but
    // deliberately does not allocate/reuse an ambiguous profiler query pair.
    commands.push_back(MakeUnique<ScopeCmd>(scope_name, true, unique_timestamp_name, _color));
    scope_stack.emplace(ScopeState{scope_name, _color, unique_timestamp_name});
}

void CommandList::PopScope() {
    if (scope_stack.empty()) {
        assert(false && "PopScope called without a matching PushScope");
        return;
    }
    const ScopeState& scope = scope_stack.top();
    assert(!scope.query_timestamp && "PopScope must match PushScope, not PushScopeWithTimeScope");
    commands.push_back(
        MakeUnique<ScopeCmd>(scope.name, false, scope.query_timestamp, scope.color)
    );
    scope_stack.pop();
}

void CommandList::PopScopeWithTimeScope() {
    if (scope_stack.empty()) {
        assert(false && "PopScopeWithTimeScope called without a matching push");
        return;
    }
    const ScopeState& scope = scope_stack.top();
    assert(
        scope.query_timestamp &&
        "PopScopeWithTimeScope must match PushScopeWithTimeScope, not PushScope"
    );
    commands.push_back(
        MakeUnique<ScopeCmd>(scope.name, false, scope.query_timestamp, scope.color)
    );
    scope_stack.pop();
}
#pragma region[ raytracing ]

void CommandList::BuildAccelerationStructures(Array<AccelerationStructureBuildParam>&& _geometries) {
    commands.emplace_back(MakeUnique<BuildAccelerationStructuresCmd>(std::move(_geometries)));
}

void CommandList::UpdateRaytracingScene(RaytracingSceneRef _scene) {
    if (!_scene) {
        return;
    }
    UniquePtr<Command> command = _scene->UpdateScene();
    if (command) {
        commands.emplace_back(std::move(command));
    }
}

void CommandList::UpdateRaytracingScene(UniquePtr<Command>&& _prepared_update) {
    assert(
        _prepared_update && _prepared_update->Type() == Command::EType::BuildTLAS &&
        "Prepared ray tracing scene update must be a BuildTLAS command"
    );
    if (_prepared_update && _prepared_update->Type() == Command::EType::BuildTLAS) {
        commands.emplace_back(std::move(_prepared_update));
    }
}

#pragma endregion
// void CommandList::SubmitArgs(ShaderPipeline& _pso, Arguments&& _args) {
//     commands.push_back(MakeUnique<SetParamsCmd>(_pso, std::move(_args)));
// }

// void CommandList::SubmitConstants(ShaderPipeline& _pso, Array<uint>&& _constants) {
//     commands.push_back(MakeUnique<SetConstantCmd>(_pso, std::move(_constants)));
// }

#pragma region[ custom commands ]

void CommandList::AddCustomCommand(UniquePtr<Command>&& _cmd, std::string_view _name) {
    commands.push_back(std::move(_cmd));
    commands.back()->name = _name;
}

#pragma endregion

void CommandList::Barriers(
    std::span<const BarrierCreateInfo> _barriers,
    EQueueType                         _src_queue,
    EQueueType                         _dst_queue
) {
    if (_barriers.empty()) {
        throw std::invalid_argument("explicit barrier batch cannot be empty");
    }
    if (_src_queue == EQueueType::Ignore || _src_queue == EQueueType::Num ||
        _src_queue != _dst_queue || _src_queue != queue_type) {
        throw std::invalid_argument(
            "explicit barrier command affinity must match its CommandList queue"
        );
    }

    const auto is_supported_ownership_queue = [](EQueueType queue) {
        return queue == EQueueType::Graphics || queue == EQueueType::Compute ||
               queue == EQueueType::Copy;
    };
    for (const BarrierCreateInfo& barrier : _barriers) {
        const auto& transfer = barrier.queue_transfer;
        switch (transfer.phase) {
            case EBarrierQueueTransferPhase::None:
                if (!((transfer.src_queue == EQueueType::Ignore &&
                       transfer.dst_queue == EQueueType::Ignore) ||
                      (transfer.src_queue == queue_type &&
                       transfer.dst_queue == queue_type))) {
                    throw std::invalid_argument(
                        "local explicit barrier endpoints must be ignored or match "
                        "the CommandList queue"
                    );
                }
                break;
            case EBarrierQueueTransferPhase::Release:
            case EBarrierQueueTransferPhase::Acquire:
                if (!is_supported_ownership_queue(transfer.src_queue) ||
                    !is_supported_ownership_queue(transfer.dst_queue) ||
                    transfer.src_queue == transfer.dst_queue) {
                    throw std::invalid_argument(
                        "queue ownership barrier requires distinct managed "
                        "Graphics/Compute/Copy endpoints"
                    );
                }
                if ((transfer.phase == EBarrierQueueTransferPhase::Release &&
                     queue_type != transfer.src_queue) ||
                    (transfer.phase == EBarrierQueueTransferPhase::Acquire &&
                     queue_type != transfer.dst_queue)) {
                    throw std::invalid_argument(
                        "queue ownership barrier was recorded on the wrong endpoint"
                    );
                }
                break;
            default:
                throw std::invalid_argument(
                    "explicit barrier carries an unknown queue-transfer phase"
                );
        }

        std::visit(
            [&](const auto& resource) {
                using TResource = std::decay_t<decltype(resource)>;
                if constexpr (std::is_same_v<TResource, TextureView>) {
                    const auto* texture = resource.GetTexture();
                    const auto aspects =
                        static_cast<uint32_t>(barrier.texture_aspects);
                    const auto available_aspects =
                        texture == nullptr ? 0u :
                        static_cast<uint32_t>(texture->GetAspectFlags());
                    if (texture == nullptr || resource.num_mips == 0 ||
                        resource.num_array == 0 ||
                        resource.mip_level >= texture->GetNumMips() ||
                        resource.num_mips >
                            texture->GetNumMips() - resource.mip_level ||
                        resource.array_layer >= texture->GetNumArray() ||
                        resource.num_array >
                            texture->GetNumArray() - resource.array_layer ||
                        aspects == 0 || (aspects & ~available_aspects) != 0) {
                        throw std::invalid_argument(
                            "explicit texture barrier has an invalid resource, "
                            "range, or aspect"
                        );
                    }
                } else {
                    static_assert(std::is_same_v<TResource, BufferView>);
                    const auto* buffer = resource.GetBuffer();
                    if (buffer == nullptr || resource.GetByteSize() == 0 ||
                        resource.GetByteOffset() > buffer->GetByteSize() ||
                        resource.GetByteSize() >
                            buffer->GetByteSize() - resource.GetByteOffset() ||
                        barrier.texture_aspects != ETextureAspectFlags::NONE ||
                        barrier.src_state.texture_layout !=
                            ETextureLayout::TEXTURE_LAYOUT_UNDEFINED ||
                        barrier.dst_state.texture_layout !=
                            ETextureLayout::TEXTURE_LAYOUT_UNDEFINED) {
                        throw std::invalid_argument(
                            "explicit buffer barrier has an invalid resource, "
                            "range, aspect, or texture layout"
                        );
                    }
                }
            },
            barrier.resource
        );
    }

    auto barrier_cmd = MakeUnique<BarrierCmd>(
        static_cast<uint>(_barriers.size()), _src_queue, _dst_queue
    );
    for (const BarrierCreateInfo& barrier : _barriers) {
        barrier_cmd->AddExplicitBarrier(barrier);
    }
    commands.push_back(std::move(barrier_cmd));
    // Recording an authoritative src/dst dependency opts this submit out of
    // the backend's legacy restore-to-preferred-state epilogue.
    resource_state_ownership = ERHIResourceStateOwnership::Explicit;
}

void CommandList::AddCallback(std::function<void()>&& _callback) {
    callbacks.emplace_back(std::move(_callback));
}

void CommandList::AddSuccessCallback(std::function<void()>&& _callback) {
    success_callbacks.emplace_back(std::move(_callback));
}

QueryToken CommandList::BeginTimestampQuery(std::string_view _name) {
    QueryToken token(
        NextNonZeroId(g_query_token_id),
        QueryKind::Timestamp,
        query_owner_id,
        _name
    );
    query_cancellation_domain.Register(token);

    // Phase 17A does not translate timestamp queries on the Copy queue. Return
    // a terminal token without manufacturing an untranslatable command.
    if (queue_type == EQueueType::Copy) {
        QueryBackendAccess::ResolveErrorIfPending(
            token,
            "timestamp queries are unsupported on Copy queues"
        );
        return token;
    }

    auto command = MakeUnique<QueryCmd>(
        token,
        QueryCmd::EOp::BeginTimestamp,
        queue_type,
        token.Name()
    );
    std::function<void()> rejection_fallback = [token] {
        QueryBackendAccess::ResolveErrorIfPending(
            token,
            "timestamp query submission did not reach GPU completion"
        );
    };

    commands.reserve(commands.size() + 1);
    callbacks.reserve(callbacks.size() + 1);
    query_tokens.reserve(query_tokens.size() + 1);
    active_query_stack.reserve(active_query_stack.size() + 1);

    const size_t command_count  = commands.size();
    const size_t callback_count = callbacks.size();
    const size_t query_count    = query_tokens.size();
    const size_t stack_depth    = active_query_stack.size();
    try {
        commands.emplace_back(std::move(command));
        callbacks.emplace_back(std::move(rejection_fallback));
        query_tokens.emplace_back(token);
        active_query_stack.emplace_back(token);
    } catch (...) {
        commands.resize(command_count);
        callbacks.resize(callback_count);
        query_tokens.resize(query_count);
        active_query_stack.resize(stack_depth);
        QueryBackendAccess::ResolveErrorIfPending(
            token,
            "timestamp query recording failed before Begin was published"
        );
        throw;
    }
    return token;
}

void CommandList::EndTimestampQuery(const QueryToken& _token) {
    if (!_token.Valid()) {
        throw std::invalid_argument(
            "EndTimestampQuery requires a valid QueryToken"
        );
    }
    if (_token.OwnerId() != query_owner_id) {
        throw std::invalid_argument(
            "EndTimestampQuery token belongs to another CommandList"
        );
    }
    if (_token.Kind() != QueryKind::Timestamp) {
        throw std::invalid_argument(
            "EndTimestampQuery token has the wrong query kind"
        );
    }
    if (queue_type == EQueueType::Copy) {
        // BeginTimestampQuery on Copy intentionally records no Begin command.
        if (_token.GetFuture().Status() == QueryStatus::Error) {
            return;
        }
        throw std::logic_error(
            "Copy queue timestamp token has no terminal capability result"
        );
    }
    if (active_query_stack.empty()) {
        throw std::logic_error(
            "EndTimestampQuery has no matching open query"
        );
    }
    if (active_query_stack.back().Id() != _token.Id()) {
        throw std::logic_error(
            "EndTimestampQuery violates strict LIFO query pairing"
        );
    }

    auto command = MakeUnique<QueryCmd>(
        _token,
        QueryCmd::EOp::EndTimestamp,
        queue_type,
        _token.Name()
    );
    commands.emplace_back(std::move(command));
    active_query_stack.pop_back();
}

void CommandList::RejectPendingQueries(std::string_view _reason) noexcept {
    const QueryPublishBatch batch = QueryBackendAccess::BeginPublishBatch();
    QueryCancellationView cancellation =
        query_cancellation_domain.GetView();
    Array<QueryToken> tokens = std::move(query_tokens);
    const bool cancellation_published =
        cancellation.PublishCancellation(_reason, batch);
    QueryBackendAccess::PublishErrorsIfPending(
        tokens, _reason, batch
    );
    if (cancellation_published) {
        cancellation.NotifyCancellation(batch);
    } else {
        cancellation.NotifyPublishedCancellation();
    }
    QueryBackendAccess::NotifyTerminals(tokens, batch);
}

void CommandList::RejectPendingSignals() noexcept {
    for (const SignalEvent& signal : signal_events) {
        auto* fence = reinterpret_cast<Fence*>(signal.timeline_handle);
        if (fence != nullptr) {
            fence->Reject(signal.value);
        }
    }
    signal_events.clear();
}

void CommandList::Signal(Fence* _fence, uint64 _signal_value) {
    if (_fence == nullptr || _signal_value == 0) {
        throw std::invalid_argument(
            "CommandList::Signal requires a valid fence and non-zero value"
        );
    }
    signal_events.emplace_back(uint64(_fence), _signal_value);
}

void CommandList::Signal(const FenceRef& _fence, uint64 _signal_value) {
    if (!_fence.IsValid() || _signal_value == 0) {
        throw std::invalid_argument(
            "CommandList::Signal requires a valid fence and non-zero value"
        );
    }

    // SignalEvent is a backend-facing raw identity. Prepare every allocation
    // before publishing either half of the pair so an allocation failure can
    // never leave a raw fence without its strong-reference keepalive.
    std::function<void()> keepalive = [fence = _fence] {};
    callbacks.reserve(callbacks.size() + 1);
    signal_events.reserve(signal_events.size() + 1);
    callbacks.emplace_back(std::move(keepalive));
    signal_events.emplace_back(uint64(_fence.Get()), _signal_value);
}

void CommandList::BeginBarriers(
    uint       _read_tex_cnt,
    uint       _write_tex_cnt,
    uint       _read_buf_cnt,
    uint       _write_buf_cnt,
    EQueueType _src_queue,
    EQueueType _dst_queue
) {
    commands.push_back(
        MakeUnique<BarrierCmd>(
            _read_tex_cnt, _write_tex_cnt, _read_buf_cnt, _write_buf_cnt, _src_queue, _dst_queue
        )
    );
    current_barriers = commands.back().get();
}

void CommandList::InnerReadBuffer(BufferView _buffer, EBufferState _state, EPassType _pass) {
    BarrierCmd* barrier = static_cast<BarrierCmd*>(current_barriers);
    barrier->ReadBuffer(_buffer, _state, _pass);
}
void CommandList::InnerWriteBuffer(BufferView _buffer, EBufferState _state, EPassType _pass) {
    BarrierCmd* barrier = static_cast<BarrierCmd*>(current_barriers);
    barrier->WriteBuffer(_buffer, _state, _pass);
}

void CommandList::InnerReadTexture(TextureView _texture, ETextureState _state, EPassType _pass) {
    BarrierCmd* barrier = static_cast<BarrierCmd*>(current_barriers);
    barrier->ReadTexture(_texture, _state, _pass);
}

void CommandList::InnerWriteTexture(TextureView _texture, ETextureState _state, EPassType _pass) {
    BarrierCmd* barrier = static_cast<BarrierCmd*>(current_barriers);
    barrier->WriteTexture(_texture, _state, _pass);
}

void CommandList::EndBarriers() {
    current_barriers = nullptr;
}

void CommandList::ImportResourcesFromQueue(
    EQueueType             _src_queue,
    Array<ImportTexture>&& _textures_to_import,
    Array<ImportBuffer>&&  _buffers_to_import
) {
    {
        // 空资源检测
        for (uint i = 0; i < _textures_to_import.size(); ++i) {
            const auto& texture_to_import = _textures_to_import[i];
            if (texture_to_import.texture.GetTexture() == nullptr) {
                LOG_WARNING(
                    "ImportResourcesFromQueue got empty texture at index {}. src_queue={}",
                    i,
                    static_cast<uint>(_src_queue)
                );
            }
        }
        for (uint i = 0; i < _buffers_to_import.size(); ++i) {
            const auto& buffer_to_import = _buffers_to_import[i];
            if (buffer_to_import.buffer.GetBuffer() == nullptr ||
                buffer_to_import.buffer.GetByteSize() == 0) {
                LOG_WARNING(
                    "ImportResourcesFromQueue got empty buffer at index {}. src_queue={}",
                    i,
                    static_cast<uint>(_src_queue)
                );
            }
        }
    }

    // QueueTransferCmd cmd(_src_queue, std::move(_textures_to_import));
    commands.emplace_back(
        MakeUnique<QueueTransferCmd>(
            _src_queue, std::move(_textures_to_import), std::move(_buffers_to_import)
        )
    );
}

void CommandList::ExportResourcesToQueue(
    EQueueType             _dst_queue,
    Array<ExportTexture>&& _textures_to_export,
    Array<ExportBuffer>&&  _buffers_to_export
) {
    {
        // 空资源检测
        for (uint i = 0; i < _textures_to_export.size(); ++i) {
            const auto& texture_to_export = _textures_to_export[i];
            if (texture_to_export.texture.GetTexture() == nullptr) {
                LOG_WARNING(
                    "ExportResourcesToQueue got empty texture at index {}. dst_queue={}",
                    i,
                    static_cast<uint>(_dst_queue)
                );
            }
        }
        for (uint i = 0; i < _buffers_to_export.size(); ++i) {
            const auto& buffer_to_export = _buffers_to_export[i];
            if (buffer_to_export.buffer.GetBuffer() == nullptr ||
                buffer_to_export.buffer.GetByteSize() == 0) {
                LOG_WARNING(
                    "ExportResourcesToQueue got empty buffer at index {}. dst_queue={}",
                    i,
                    static_cast<uint>(_dst_queue)
                );
            }
        }
    }

    commands.emplace_back(
        MakeUnique<QueueTransferCmd>(
            _dst_queue, std::move(_textures_to_export), std::move(_buffers_to_export)
        )
    );
}

ArrayArgReference CommandList::RegisterArgs(ArrayArguments&& _args) {
    cached_args.push_back(std::move(_args));
    return ArrayArgReference(cached_args.size() - 1);
}

} // namespace Moer::Render
