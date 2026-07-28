#include "rendergraph/RenderGraph.h"

#include "Core.h"
#include "misc/Assert.h"
#include "rhi/RHICommand.h"
#include "rhi/RHIImpl.h"
#include "taskgraph/TaskGraph.h"
#include "trace/Trace.h"

#include <algorithm>
#include <mutex>
#include <optional>
#include <utility>

namespace Moer::Render {
namespace {

struct RGPreparedRecordedBatch {
    uint32_t                       batch_index{RGPass::invalid_pass};
    SharedPtr<Render::CommandList> command_list{};
    GraphEventRef                  record_complete_event{nullptr};
};

std::mutex& LastRenderGraphEventsMutex() {
    static std::mutex mutex{};
    return mutex;
}

Moer::Array<GraphEventRef>& LastRenderGraphEventsStorage() {
    static Moer::Array<GraphEventRef> events{};
    return events;
}

void WaitLastRenderGraphEvents() {
    Moer::Array<GraphEventRef> pending_events{};
    {
        std::scoped_lock lock(LastRenderGraphEventsMutex());
        pending_events = std::move(LastRenderGraphEventsStorage());
    }

    for (const GraphEventRef& event : pending_events) {
        if (event && !event->IsComplete()) {
            event->Wait(EThread::UNKNOWN_THREAD);
        }
    }
}

void PublishLastRenderGraphEvents(Moer::Array<GraphEventRef>&& events) {
    std::scoped_lock lock(LastRenderGraphEventsMutex());
    LastRenderGraphEventsStorage() = std::move(events);
}

static constexpr uint32_t invalid_pass = RGPass::invalid_pass;
void AppendUniqueSyncPoint(Moer::Array<Render::SyncPointRef>& sync_points, const Render::SyncPointRef& sync_point) {
    if (!sync_point) {
        return;
    }
    for (const Render::SyncPointRef& existing : sync_points) {
        if (existing == sync_point) {
            return;
        }
    }
    sync_points.push_back(sync_point);
}

void AppendUniqueIndex(Moer::Array<uint32_t>& indices, uint32_t index) {
    for (uint32_t existing : indices) {
        if (existing == index) {
            return;
        }
    }
    indices.push_back(index);
}

bool BatchContainsPass(const RGCompiledExecutionBatch& batch, uint32_t pass_index) {
    return pass_index >= batch.first_pass && pass_index < batch.first_pass + batch.pass_count;
}

void EmitRGPassTransitions(RHICommandList& cmd_list, const RGPass& pass);

StringView RGScopeNameAt(StringView graph_name, const Moer::Array<String>& pass_scopes, size_t index) {
    return index == 0 ? graph_name : StringView(pass_scopes[index - 1]);
}

size_t RGScopeTargetCount(StringView graph_name, const Moer::Array<String>& pass_scopes) {
    return graph_name.empty() ? pass_scopes.size() : pass_scopes.size() + 1;
}

StringView RGScopeTargetName(StringView graph_name, const Moer::Array<String>& pass_scopes, size_t index) {
    if (graph_name.empty()) {
        return StringView(pass_scopes[index]);
    }
    return RGScopeNameAt(graph_name, pass_scopes, index);
}

void SyncRGProfilerScopes(
    RHICommandList&            cmd_list,
    Moer::Array<String>&       open_scopes,
    StringView                 graph_name,
    const Moer::Array<String>& pass_scopes
) {
    const size_t target_count = RGScopeTargetCount(graph_name, pass_scopes);
    size_t       common_count = 0;
    while (common_count < open_scopes.size() && common_count < target_count &&
           StringView(open_scopes[common_count]) == RGScopeTargetName(graph_name, pass_scopes, common_count)) {
        ++common_count;
    }

    while (open_scopes.size() > common_count) {
        cmd_list.PopScopeWithTimeScope();
        open_scopes.pop_back();
    }
    for (size_t scope_index = common_count; scope_index < target_count; ++scope_index) {
        const StringView scope_name = RGScopeTargetName(graph_name, pass_scopes, scope_index);
        if (scope_name.empty()) {
            continue;
        }
        cmd_list.PushScopeWithTimeScope(scope_name);
        open_scopes.emplace_back(scope_name);
    }
}

void CloseRGProfilerScopes(RHICommandList& cmd_list, Moer::Array<String>& open_scopes) {
    while (!open_scopes.empty()) {
        cmd_list.PopScopeWithTimeScope();
        open_scopes.pop_back();
    }
}

bool IsCrossQueueTransition(Render::EQueueType src_queue, Render::EQueueType dst_queue) {
    return src_queue != Render::EQueueType::Ignore && dst_queue != Render::EQueueType::Ignore &&
           src_queue != dst_queue;
}

void RenameTextureForDebug(const RGTexture& texture) {
    if (texture.IsAllocated()) {
        texture.RHI()->SetName(texture.name);
    }
}

void RenameBufferForDebug(const RGBuffer& buffer) {
    if (buffer.IsAllocated()) {
        buffer.RHI()->SetName(buffer.name);
    }
}

void AddTextureQueueImport(RHICommandList& cmd_list, const RGCompiledTextureTransition& transition) {
    RenameTextureForDebug(*transition.texture);
    Moer::Array<ImportTexture> textures{};
    textures.emplace_back(ImportTexture{
        .texture = transition.texture->GetView(transition.range),
        .state = transition.state,
        .access_write = RGTextureStateWrites(transition.state)
    });
    Moer::Array<ImportBuffer> buffers{};
    cmd_list.AddCustomCommand(
        MakeUnique<QueueTransferCmd>(transition.src_queue, std::move(textures), std::move(buffers)),
        MOER_TEXT("RG.QueueImport")
    );
}

void AddBufferQueueImport(RHICommandList& cmd_list, const RGCompiledBufferTransition& transition) {
    RenameBufferForDebug(*transition.buffer);
    Moer::Array<ImportTexture> textures{};
    Moer::Array<ImportBuffer>  buffers{};
    buffers.emplace_back(ImportBuffer{
        .buffer = transition.buffer->GetView(transition.range),
        .state = transition.state,
        .access_write = RGBufferStateWrites(transition.state)
    });
    cmd_list.AddCustomCommand(
        MakeUnique<QueueTransferCmd>(transition.src_queue, std::move(textures), std::move(buffers)),
        MOER_TEXT("RG.QueueImport")
    );
}

void AddTextureQueueExport(RHICommandList& cmd_list, const RGCompiledTextureTransition& transition) {
    RenameTextureForDebug(*transition.texture);
    Moer::Array<ExportTexture> textures{};
    textures.emplace_back(ExportTexture{
        .texture = transition.texture->GetView(transition.range),
        .state = transition.src_state
    });
    Moer::Array<ExportBuffer> buffers{};
    cmd_list.AddCustomCommand(
        MakeUnique<QueueTransferCmd>(transition.dst_queue, std::move(textures), std::move(buffers)),
        MOER_TEXT("RG.QueueExport")
    );
}

void AddBufferQueueExport(RHICommandList& cmd_list, const RGCompiledBufferTransition& transition) {
    RenameBufferForDebug(*transition.buffer);
    Moer::Array<ExportTexture> textures{};
    Moer::Array<ExportBuffer>  buffers{};
    buffers.emplace_back(ExportBuffer{
        .buffer = transition.buffer->GetView(transition.range),
        .state = transition.src_state
    });
    cmd_list.AddCustomCommand(
        MakeUnique<QueueTransferCmd>(transition.dst_queue, std::move(textures), std::move(buffers)),
        MOER_TEXT("RG.QueueExport")
    );
}

void EmitBatchQueueImports(
    RHICommandList&                         cmd_list,
    const RGCompiledExecutionBatch&         batch,
    const Moer::Array<RGPass>&              passes,
    const Moer::Array<RGCompiledHazardEdge>& hazard_edges
) {
    for (const RGCompiledHazardEdge& edge : hazard_edges) {
        if (!BatchContainsPass(batch, edge.dst_pass) || !IsCrossQueueTransition(edge.src_queue, edge.dst_queue)) {
            continue;
        }

        const RGPass& dst_pass = passes[edge.dst_pass];
        if (edge.resource_kind == ERGResourceKind::Texture) {
            for (const RGCompiledTextureTransition& transition : dst_pass.compile.texture_transitions) {
                if (transition.texture == edge.resource && transition.src_queue == edge.src_queue &&
                    transition.dst_queue == edge.dst_queue) {
                    AddTextureQueueImport(cmd_list, transition);
                }
            }
            continue;
        }
        if (edge.resource_kind != ERGResourceKind::Buffer) {
            continue;
        }

        for (const RGCompiledBufferTransition& transition : dst_pass.compile.buffer_transitions) {
            if (transition.buffer == edge.resource && transition.src_queue == edge.src_queue &&
                transition.dst_queue == edge.dst_queue) {
                AddBufferQueueImport(cmd_list, transition);
            }
        }
    }
}

void EmitBatchQueueExports(
    RHICommandList&                         cmd_list,
    const RGCompiledExecutionBatch&         batch,
    const Moer::Array<RGPass>&              passes,
    const Moer::Array<RGCompiledHazardEdge>& hazard_edges
) {
    for (const RGCompiledHazardEdge& edge : hazard_edges) {
        if (!BatchContainsPass(batch, edge.src_pass) || !IsCrossQueueTransition(edge.src_queue, edge.dst_queue)) {
            continue;
        }

        const RGPass& dst_pass = passes[edge.dst_pass];
        if (edge.resource_kind == ERGResourceKind::Texture) {
            for (const RGCompiledTextureTransition& transition : dst_pass.compile.texture_transitions) {
                if (transition.texture == edge.resource && transition.src_queue == edge.src_queue &&
                    transition.dst_queue == edge.dst_queue) {
                    AddTextureQueueExport(cmd_list, transition);
                }
            }
            continue;
        }
        if (edge.resource_kind != ERGResourceKind::Buffer) {
            continue;
        }

        for (const RGCompiledBufferTransition& transition : dst_pass.compile.buffer_transitions) {
            if (transition.buffer == edge.resource && transition.src_queue == edge.src_queue &&
                transition.dst_queue == edge.dst_queue) {
                AddBufferQueueExport(cmd_list, transition);
            }
        }
    }
}

void CollectBatchWaitSyncPoints(
    Moer::Array<Render::SyncPointRef>&       wait_sync_points,
    const RGCompiledExecutionBatch&          batch,
    const Moer::Array<Render::SyncPointRef>& signal_sync_point_by_batch
) {
    wait_sync_points.clear();
    for (uint32_t producer_batch : batch.wait_sync_point_batches) {
        if (producer_batch >= signal_sync_point_by_batch.size()) {
            continue;
        }
        AppendUniqueSyncPoint(wait_sync_points, signal_sync_point_by_batch[producer_batch]);
    }
}

void CollectBatchExplicitStates(
    Moer::Array<Render::TrackedTextureState>& textures,
    Moer::Array<Render::TrackedBufferState>&  buffers,
    const RGCompiledExecutionBatch&           batch,
    const Moer::Array<RGPass>&                passes
) {
    textures.clear();
    buffers.clear();

    for (uint32_t offset = 0; offset < batch.pass_count; ++offset) {
        const RGPass& batch_pass = passes[batch.first_pass + offset];
        for (const RGCompiledTextureTransition& transition : batch_pass.compile.texture_transitions) {
            if (transition.texture == nullptr || !transition.texture->IsAllocated()) {
                continue;
            }
            textures.emplace_back(Render::TrackedTextureState{
                .texture = transition.texture->GetView(transition.range),
                .state = transition.state,
                .owner_queue = transition.dst_queue,
                .access_write = RGTextureStateWrites(transition.state)
            });
        }
        for (const RGCompiledBufferTransition& transition : batch_pass.compile.buffer_transitions) {
            if (transition.buffer == nullptr || !transition.buffer->IsAllocated()) {
                continue;
            }
            buffers.emplace_back(Render::TrackedBufferState{
                .buffer = transition.buffer->GetView(transition.range),
                .state = transition.state,
                .owner_queue = transition.dst_queue,
                .access_write = RGBufferStateWrites(transition.state)
            });
        }
    }
}

void RecordExecutionBatch(
    RHICommandList&                command_list,
    const RGCompiledExecutionBatch& batch,
    Moer::Array<RGPass>&           passes,
    const RGCompiledPlan&          compiled_plan,
    RGContext                      context,
    StringView                     graph_name
) {
    Moer::Array<String> open_scopes{};
    SyncRGProfilerScopes(command_list, open_scopes, graph_name, {});
    EmitBatchQueueImports(command_list, batch, passes, compiled_plan.hazard_edges);
    for (uint32_t offset = 0; offset < batch.pass_count; ++offset) {
        RGPass& batch_pass = passes[batch.first_pass + offset];
        SyncRGProfilerScopes(command_list, open_scopes, graph_name, batch_pass.event_scopes);
        EmitRGPassTransitions(command_list, batch_pass);
        if (!batch_pass.name.empty()) {
            command_list.PushScopeWithTimeScope(batch_pass.name);
        }
        batch_pass.execute(&command_list, context);
        if (!batch_pass.name.empty()) {
            command_list.PopScopeWithTimeScope();
        }
    }
    SyncRGProfilerScopes(command_list, open_scopes, graph_name, {});
    EmitBatchQueueExports(command_list, batch, passes, compiled_plan.hazard_edges);
    CloseRGProfilerScopes(command_list, open_scopes);
}

template<typename TExecutionStatePtr>
void PrepareRecordedBatchCommandList(
    Render::CommandList&                     command_list,
    const RGCompiledExecutionBatch&          batch,
    const TExecutionStatePtr&                execution_state,
    const Moer::Array<Render::SyncPointRef>& signal_sync_point_by_batch,
    uint32_t                                 batch_index
) {
    Moer::Array<Render::SyncPointRef>      wait_sync_points{};
    Moer::Array<Render::TrackedTextureState> explicit_textures{};
    Moer::Array<Render::TrackedBufferState>  explicit_buffers{};
    CollectBatchWaitSyncPoints(wait_sync_points, batch, signal_sync_point_by_batch);
    CollectBatchExplicitStates(explicit_textures, explicit_buffers, batch, execution_state->passes);

    command_list.SetExplicitTrackedState(std::move(explicit_textures), std::move(explicit_buffers));
    for (const Render::SyncPointRef& wait_sync_point : wait_sync_points) {
        command_list.Wait(wait_sync_point);
    }
    if (batch_index < signal_sync_point_by_batch.size()) {
        command_list.Signal(signal_sync_point_by_batch[batch_index]);
    }
    command_list.AddCallback([execution_state]() {});
}

void EmitRGPassTransitions(RHICommandList& cmd_list, const RGPass& pass) {
    const EPassType pass_type = RGPassType(pass.flags);

    for (const RGCompiledTextureTransition& transition : pass.compile.texture_transitions) {
        if (transition.texture == nullptr || !transition.texture->IsAllocated()) {
            continue;
        }
        RenameTextureForDebug(*transition.texture);
        const Render::EQueueType src_queue = transition.src_queue == Render::EQueueType::Ignore ?
                                                 transition.dst_queue :
                                                 transition.src_queue;
        if (IsCrossQueueTransition(src_queue, transition.dst_queue)) {
            continue;
        }
        const std::array<Render::BarrierCreateInfo, 1> barriers{Render::BarrierCreateInfo::Transition(
            transition.texture->GetView(transition.range),
            Render::MakeBarrierState(transition.src_state, pass_type),
            Render::MakeBarrierState(transition.state, pass_type)
        )};
        cmd_list.Barriers(barriers, src_queue, transition.dst_queue, Render::ETrackedStateUpdateMode::Skip);
    }

    for (const RGCompiledBufferTransition& transition : pass.compile.buffer_transitions) {
        if (transition.buffer == nullptr || !transition.buffer->IsAllocated()) {
            continue;
        }
        RenameBufferForDebug(*transition.buffer);
        const Render::EQueueType src_queue = transition.src_queue == Render::EQueueType::Ignore ?
                                                 transition.dst_queue :
                                                 transition.src_queue;
        if (IsCrossQueueTransition(src_queue, transition.dst_queue)) {
            continue;
        }
        const std::array<Render::BarrierCreateInfo, 1> barriers{Render::BarrierCreateInfo::Transition(
            transition.buffer->GetView(transition.range),
            Render::MakeBarrierState(transition.src_state, pass_type),
            Render::MakeBarrierState(transition.state, pass_type)
        )};
        cmd_list.Barriers(barriers, src_queue, transition.dst_queue, Render::ETrackedStateUpdateMode::Skip);
    }


}

PooledTextureRef MakeExternalPooledTexture(StringView name, Render::TextureRef texture) {
    assert(texture.Get() != nullptr);
    return MakeShared<PooledTexture>(name, texture->GetInfo(), std::move(texture), nullptr, false);
}

PooledBufferRef MakeExternalPooledBuffer(StringView name, Render::BufferRef buffer) {
    assert(buffer.Get() != nullptr);
    return MakeShared<PooledBuffer>(name, buffer->GetInfo(), std::move(buffer), nullptr, false);
}

bool IsValidQueue(Render::EQueueType queue) {
    return queue != Render::EQueueType::Ignore && queue != Render::EQueueType::Num;
}

EPassType PassTypeForQueue(Render::EQueueType queue) {
    switch (queue) {
        case Render::EQueueType::Compute:
            return EPassType::Compute;
        case Render::EQueueType::Copy:
            return EPassType::Copy;
        case Render::EQueueType::Graphics:
        case Render::EQueueType::Ignore:
        case Render::EQueueType::Num:
        default:
            return EPassType::Graphics;
    }
}

void SeedImportedTextureStateRange(RGTexture& texture, RGTextureStateRange& state_range) {
    if (!texture.imported || !texture.IsAllocated()) {
        return;
    }

    auto* rhi_texture = texture.RHI().Get();
    if (rhi_texture == nullptr) {
        return;
    }

    bool                           initialized = false;
    Render::TexturePersistentState initial_state{};
    for (uint32_t mip = state_range.range.mip_min; mip < state_range.range.mip_min + state_range.range.mip_count;
         ++mip) {
        for (uint32_t layer = state_range.range.array_min;
             layer < state_range.range.array_min + state_range.range.array_count;
             ++layer) {
            const Render::TexturePersistentState persistent = rhi_texture->GetPersistentState();
            if (!persistent.known) {
                return;
            }
            if (!initialized) {
                initial_state = persistent;
                initialized   = true;
                continue;
            }
            if (persistent.state != initial_state.state || persistent.owner_queue != initial_state.owner_queue ||
                persistent.last_access_kind != initial_state.last_access_kind) {
                return;
            }
        }
    }

    if (!initialized) {
        return;
    }
    state_range.state = initial_state.state;
    state_range.queue = initial_state.owner_queue;
}

void SeedImportedBufferStateRanges(RGBuffer& buffer) {
    if (!buffer.imported || !buffer.IsAllocated()) {
        return;
    }

    auto* rhi_buffer = buffer.RHI().Get();
    if (rhi_buffer == nullptr) {
        return;
    }

    const Render::BufferPersistentState persistent = rhi_buffer->GetPersistentState();
    if (!persistent.known) {
        return;
    }

    for (RGBufferStateRange& state_range : buffer.state_ranges) {
        state_range.state = persistent.state;
        state_range.queue = persistent.owner_queue;
    }
}

void SeedAliasTextureStateRanges(RGTexture& texture) {
    if (texture.alias_initial_state_ranges.empty()) {
        return;
    }
    for (RGTextureStateRange& state_range : texture.state_ranges) {
        for (const RGTextureStateRange& alias_range : texture.alias_initial_state_ranges) {
            if (!alias_range.range.Contains(state_range.range)) {
                continue;
            }
            state_range.state     = alias_range.state;
            state_range.queue     = alias_range.queue;
            state_range.last_pass = alias_range.last_pass;
            state_range.alias_initial = true;
            break;
        }
    }
}

void SeedAliasBufferStateRanges(RGBuffer& buffer) {
    if (buffer.alias_initial_state_ranges.empty()) {
        return;
    }
    for (RGBufferStateRange& state_range : buffer.state_ranges) {
        for (const RGBufferStateRange& alias_range : buffer.alias_initial_state_ranges) {
            if (!alias_range.range.Contains(state_range.range)) {
                continue;
            }
            state_range.state     = alias_range.state;
            state_range.queue     = alias_range.queue;
            state_range.last_pass = alias_range.last_pass;
            state_range.alias_initial = true;
            break;
        }
    }
}

size_t QueueIndex(Render::EQueueType queue) {
    assert(IsValidQueue(queue));
    return static_cast<size_t>(queue);
}

void RecordLatestPass(uint32_t& slot, uint32_t pass_index) {
    if (slot == invalid_pass || slot < pass_index) {
        slot = pass_index;
    }
}

void RecordEarliestPass(uint32_t& slot, uint32_t pass_index) {
    if (slot == invalid_pass || pass_index < slot) {
        slot = pass_index;
    }
}

uint64_t MakeHazardKey(uint32_t src_pass, uint32_t dst_pass, const RGResource& resource) {
    assert(src_pass < (1u << 20));
    assert(dst_pass < (1u << 20));
    assert(resource.Index() < (1u << 20));
    return (static_cast<uint64_t>(src_pass) << 44) | (static_cast<uint64_t>(dst_pass) << 24) |
           (static_cast<uint64_t>(resource.kind) << 20) | static_cast<uint64_t>(resource.Index());
}

void AddBoundary(Moer::Array<uint32_t>& boundaries, uint32_t value) {
    boundaries.push_back(value);
}

void AddBoundary(Moer::Array<uint64_t>& boundaries, uint64_t value) {
    boundaries.push_back(value);
}

template<typename T>
void SortUnique(Moer::Array<T>& values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

RGTextureRange NormalizeTextureRange(const RGTexture& texture, RGTextureRange range) {
    const RGTextureDesc& desc        = texture.Desc();
    const uint32_t       mip_count   = desc.num_mips == 0 ? 1u : static_cast<uint32_t>(desc.num_mips);
    const uint32_t       array_count = desc.array_size == 0 ? 1u : static_cast<uint32_t>(desc.array_size);
    if (range.aspect == ETextureAspectFlags::NONE) {
        range.aspect = desc.aspect_flags == ETextureAspectFlags::NONE ? ETextureAspectFlags::COLOR : desc.aspect_flags;
    }
    if (range.mip_count == 0) {
        range.mip_count = mip_count - range.mip_min;
    }
    if (range.array_count == 0) {
        range.array_count = array_count - range.array_min;
    }
    assert(range.mip_min < mip_count);
    assert(range.array_min < array_count);
    assert(range.mip_min + range.mip_count <= mip_count);
    assert(range.array_min + range.array_count <= array_count);
    return range;
}

uint64_t BufferByteSize(const RGBuffer& buffer) {
    const RGBufferDesc& desc = buffer.Desc();
    return desc.size * desc.stride;
}

RGBufferRange NormalizeBufferRange(const RGBuffer& buffer, RGBufferRange range) {
    const uint64_t byte_size = BufferByteSize(buffer);
    if (range.IsWholeResource()) {
        return RGBufferRange{0, byte_size};
    }
    assert(range.size > 0);
    assert(range.offset + range.size <= byte_size);
    return range;
}

Moer::Array<ETextureAspectFlags> TextureAspects(ETextureAspectFlags aspect_flags) {
    static constexpr ETextureAspectFlags known_aspects[] = {
        ETextureAspectFlags::COLOR,
        ETextureAspectFlags::DEPTH_SLICE,
        ETextureAspectFlags::STENCIL_SLICE,
        ETextureAspectFlags::META_DATA,
        ETextureAspectFlags::PLANE_0,
        ETextureAspectFlags::PLANE_1,
        ETextureAspectFlags::PLANE_2,
        ETextureAspectFlags::MEMORY_PLANE_0,
        ETextureAspectFlags::MEMORY_PLANE_1,
        ETextureAspectFlags::MEMORY_PLANE_2,
        ETextureAspectFlags::MEMORY_PLANE_3
    };

    Moer::Array<ETextureAspectFlags> aspects{};
    for (ETextureAspectFlags aspect : known_aspects) {
        if (EnumHasAnyFlag(aspect_flags, aspect)) {
            aspects.push_back(aspect);
        }
    }
    if (aspects.empty()) {
        aspects.push_back(ETextureAspectFlags::COLOR);
    }
    return aspects;
}

} // namespace

bool RGPassHasQueue(ERGPassFlags flags) {
    uint32_t count = 0;
    count += EnumHasAnyFlag(flags, ERGPassFlags::Graphics) ? 1 : 0;
    count += EnumHasAnyFlag(flags, ERGPassFlags::Compute) ? 1 : 0;
    count += EnumHasAnyFlag(flags, ERGPassFlags::Copy) ? 1 : 0;
    return count == 1;
}

bool RGPassHasValidQueueFlags(ERGPassFlags flags) {
    return flags == ERGPassFlags::None || RGPassHasQueue(flags);
}

bool RGPassIsSerial(ERGPassFlags flags) {
    return EnumHasAnyFlag(flags, ERGPassFlags::Serial);
}

Render::EQueueType RGPassQueue(ERGPassFlags flags) {
    assert(RGPassHasValidQueueFlags(flags));
    if (flags == ERGPassFlags::None) {
        return Render::EQueueType::Ignore;
    }
    if (EnumHasAnyFlag(flags, ERGPassFlags::Compute)) {
        return Render::EQueueType::Compute;
    }
    if (EnumHasAnyFlag(flags, ERGPassFlags::Copy)) {
        return Render::EQueueType::Copy;
    }
    return Render::EQueueType::Graphics;
}

EPassType RGPassType(ERGPassFlags flags) {
    assert(RGPassHasQueue(flags));
    if (EnumHasAnyFlag(flags, ERGPassFlags::RaytracingShader)) {
        return EPassType::Raytracing;
    }
    if (EnumHasAnyFlag(flags, ERGPassFlags::ComputeShader)) {
        return EPassType::Compute;
    }
    if (EnumHasAnyFlag(flags, ERGPassFlags::Compute)) {
        return EPassType::Compute;
    }
    if (EnumHasAnyFlag(flags, ERGPassFlags::Copy)) {
        return EPassType::Copy;
    }
    return EPassType::Graphics;
}

RGTransientResourceAllocator::RGTransientResourceAllocator(
    PooledTexturePool& texture_pool,
    PooledBufferPool&  buffer_pool
) :
    m_texture_pool(texture_pool),
    m_buffer_pool(buffer_pool) {}

RGTransientResourceAllocator& RGTransientResourceAllocator::Global() {
    static RGTransientResourceAllocator allocator{PooledTexturePool::Global(), PooledBufferPool::Global()};
    return allocator;
}

void RGTransientResourceAllocator::Allocate(RenderGraph& graph) {
    WaitLastRenderGraphEvents();

    struct TextureAliasSlot {
        PooledTextureRef                  resource{};
        uint32_t                          available_after{invalid_pass};
        Moer::Array<RGTextureStateRange>  final_ranges{};
    };

    struct BufferAliasSlot {
        PooledBufferRef                  resource{};
        uint32_t                         available_after{invalid_pass};
        Moer::Array<RGBufferStateRange>  final_ranges{};
    };

    const auto reset_transient_metadata = [&]() {
        for (Moer::UniquePtr<RGTexture>& texture : graph.m_textures) {
            if (!texture->imported) {
                texture->compile.Reset();
                texture->transient.ResetCompileState();
                texture->alias_initial_state_ranges.clear();
            }
        }
        for (Moer::UniquePtr<RGBuffer>& buffer : graph.m_buffers) {
            if (!buffer->imported) {
                buffer->compile.Reset();
                buffer->transient.ResetCompileState();
                buffer->alias_initial_state_ranges.clear();
            }
        }
    };

    const auto record_texture_access = [&](uint32_t pass_index, const RGTextureAccess& access) {
        RGTexture& texture = *access.texture;
        if (texture.imported) {
            return;
        }
        texture.compile.RecordAccess(pass_index);
        const RGTextureRange access_range = NormalizeTextureRange(texture, access.range);
        for (RGTextureStateRange& state_range : texture.state_ranges) {
            if (!state_range.range.Overlaps(access_range)) {
                continue;
            }
            state_range.state     = access.state;
            state_range.queue     = access.queue;
            state_range.last_pass = pass_index;
        }
    };

    const auto record_buffer_access = [&](uint32_t pass_index, const RGBufferAccess& access) {
        RGBuffer& buffer = *access.buffer;
        if (buffer.imported) {
            return;
        }
        buffer.compile.RecordAccess(pass_index);
        const RGBufferRange access_range = NormalizeBufferRange(buffer, access.range);
        for (RGBufferStateRange& state_range : buffer.state_ranges) {
            if (!state_range.range.Overlaps(access_range)) {
                continue;
            }
            state_range.state     = access.state;
            state_range.queue     = access.queue;
            state_range.last_pass = pass_index;
        }
    };

    reset_transient_metadata();
    for (uint32_t pass_index = 0; pass_index < graph.m_passes.size(); ++pass_index) {
        const RGPass& pass = graph.m_passes[pass_index];
        if (pass.main_thread) {
            continue;
        }
        for (const RGTextureAccess& access : pass.texture_accesses) {
            record_texture_access(pass_index, access);
        }
        for (const RGBufferAccess& access : pass.buffer_accesses) {
            record_buffer_access(pass_index, access);
        }
    }

    Moer::Array<RGTexture*> texture_resources{};
    Moer::Array<RGBuffer*>  buffer_resources{};

    for (Moer::UniquePtr<RGTexture>& texture : graph.m_textures) {
        if (texture->imported || texture->IsAllocated() || texture->compile.first_pass == invalid_pass) {
            continue;
        }
        texture_resources.push_back(texture.get());
    }
    for (Moer::UniquePtr<RGBuffer>& buffer : graph.m_buffers) {
        if (buffer->imported || buffer->IsAllocated() || buffer->compile.first_pass == invalid_pass) {
            continue;
        }
        buffer_resources.push_back(buffer.get());
    }

    std::sort(texture_resources.begin(), texture_resources.end(), [](const RGTexture* lhs, const RGTexture* rhs) {
        if (lhs->compile.first_pass != rhs->compile.first_pass) {
            return lhs->compile.first_pass < rhs->compile.first_pass;
        }
        return lhs->Index() < rhs->Index();
    });
    std::sort(buffer_resources.begin(), buffer_resources.end(), [](const RGBuffer* lhs, const RGBuffer* rhs) {
        if (lhs->compile.first_pass != rhs->compile.first_pass) {
            return lhs->compile.first_pass < rhs->compile.first_pass;
        }
        return lhs->Index() < rhs->Index();
    });

    Moer::Array<TextureAliasSlot> texture_slots{};
    for (RGTexture* texture : texture_resources) {
        TextureAliasSlot* alias_slot = nullptr;
        if (!texture->exported) {
            for (TextureAliasSlot& slot : texture_slots) {
                if (slot.available_after < texture->compile.first_pass && slot.resource &&
                    slot.resource->Desc() == texture->Desc()) {
                    alias_slot = &slot;
                    break;
                }
            }
        }
        if (alias_slot == nullptr) {
            PooledTextureRef resource = m_texture_pool.Allocate(texture->name, texture->Desc());
            texture->Bind(resource);
            if (!texture->exported) {
                texture_slots.push_back(
                    TextureAliasSlot{
                        .resource = std::move(resource),
                        .available_after = texture->compile.last_pass,
                        .final_ranges = texture->state_ranges
                    }
                );
            }
            continue;
        }

        texture->Bind(alias_slot->resource);
        texture->alias_initial_state_ranges = alias_slot->final_ranges;
        alias_slot->available_after = texture->compile.last_pass;
        alias_slot->final_ranges    = texture->state_ranges;
    }

    Moer::Array<BufferAliasSlot> buffer_slots{};
    for (RGBuffer* buffer : buffer_resources) {
        BufferAliasSlot* alias_slot = nullptr;
        if (!buffer->exported) {
            for (BufferAliasSlot& slot : buffer_slots) {
                if (slot.available_after < buffer->compile.first_pass && slot.resource &&
                    slot.resource->Desc() == buffer->Desc()) {
                    alias_slot = &slot;
                    break;
                }
            }
        }
        if (alias_slot == nullptr) {
            PooledBufferRef resource = m_buffer_pool.Allocate(buffer->name, buffer->Desc());
            buffer->Bind(resource);
            if (!buffer->exported) {
                buffer_slots.push_back(
                    BufferAliasSlot{
                        .resource = std::move(resource),
                        .available_after = buffer->compile.last_pass,
                        .final_ranges = buffer->state_ranges
                    }
                );
            }
            continue;
        }

        buffer->Bind(alias_slot->resource);
        buffer->alias_initial_state_ranges = alias_slot->final_ranges;
        alias_slot->available_after = buffer->compile.last_pass;
        alias_slot->final_ranges    = buffer->state_ranges;
    }
}

void RGTransientResourceAllocator::Release(RenderGraph& graph) {
    for (Moer::UniquePtr<RGTexture>& texture : graph.m_textures) {
        texture->ReleaseTransient();
    }
    for (Moer::UniquePtr<RGBuffer>& buffer : graph.m_buffers) {
        buffer->ReleaseTransient();
    }
}

RenderGraph::RenderGraph() : RenderGraph(MOER_TEXT("RenderGraph")) {}

RenderGraph::RenderGraph(StringView name) : m_name(name.empty() ? MOER_TEXT("RenderGraph") : name) {}

RenderGraph::~RenderGraph() {
    Reset();
}

RGEventScope::RGEventScope(RenderGraph& graph, String name) {
    if (!name.empty()) {
        m_graph = &graph;
        m_graph->PushEventScope(std::move(name));
    }
}

RGEventScope::~RGEventScope() {
    if (m_graph != nullptr) {
        m_graph->PopEventScope();
    }
}

void RenderGraph::PushEventScope(String name) {
    assert(!m_compiled && "RenderGraph event scopes must be created before compile");
    ValidateBuildThread();
    assert(!name.empty() && "RenderGraph event scope name must not be empty");
    if (name.empty()) {
        return;
    }
    m_scope_stack.push_back(std::move(name));
}

void RenderGraph::PopEventScope() {
    assert(!m_compiled && "RenderGraph event scopes must end before compile");
    ValidateBuildThread();
    assert(!m_scope_stack.empty() && "RenderGraph event scope stack underflow");
    if (!m_scope_stack.empty()) {
        m_scope_stack.pop_back();
    }
}

RenderGraph::RGExecutionState::~RGExecutionState() {
    for (RGAllocation& allocation : allocations) {
        if (allocation.ptr && allocation.destroy) {
            allocation.destroy(allocation.ptr);
        }
    }
}

RGTexture* RenderGraph::CreateTexture(StringView name, const RGTextureDesc& desc) {
    assert(!m_compiled);
    return AddTexture(MakeUnique<RGTexture>(name, desc));
}

RGBuffer* RenderGraph::CreateBuffer(StringView name, const RGBufferDesc& desc) {
    assert(!m_compiled);
    return AddBuffer(MakeUnique<RGBuffer>(name, desc));
}

RGTexture* RenderGraph::RegisterTexture(
    StringView         name,
    PooledTextureRef   texture,
    Render::EQueueType owner_queue
) {
    assert(!m_compiled);
    assert(texture && texture->IsAllocated());
    RGTexture* resource   = AddTexture(MakeUnique<RGTexture>(name, std::move(texture), true));
    resource->owner_queue = owner_queue;
    return resource;
}

RGBuffer* RenderGraph::RegisterBuffer(
    StringView         name,
    PooledBufferRef    buffer,
    Render::EQueueType owner_queue
) {
    assert(!m_compiled);
    assert(buffer && buffer->IsAllocated());
    RGBuffer* resource    = AddBuffer(MakeUnique<RGBuffer>(name, std::move(buffer), true));
    resource->owner_queue = owner_queue;
    return resource;
}

RGTexture* RenderGraph::ImportTexture(
    StringView         name,
    Render::TextureRef texture,
    Render::EQueueType owner_queue
) {
    assert(!m_compiled);
    assert(texture);
    return RegisterTexture(name, MakeExternalPooledTexture(name, std::move(texture)), owner_queue);
}

RGBuffer* RenderGraph::ImportBuffer(StringView name, Render::BufferRef buffer, Render::EQueueType owner_queue) {
    assert(!m_compiled);
    assert(buffer);
    return RegisterBuffer(name, MakeExternalPooledBuffer(name, std::move(buffer)), owner_queue);
}

void RenderGraph::ExportTexture(
    RGTexture*            texture,
    Render::ETextureState final_state,
    Render::EQueueType    owner_queue
) {
    assert(texture != nullptr);
    assert(texture->kind == ERGResourceKind::Texture);
    assert(final_state != Render::ETextureState::UNDEFINED);
    texture->exported    = true;
    texture->final_state = final_state;
    texture->owner_queue = owner_queue;
}

void RenderGraph::ExportBuffer(
    RGBuffer*            buffer,
    Render::EBufferState final_state,
    Render::EQueueType   owner_queue
) {
    assert(buffer != nullptr);
    assert(buffer->kind == ERGResourceKind::Buffer);
    assert(final_state != Render::EBufferState::UNDEFINED);
    buffer->exported    = true;
    buffer->final_state = final_state;
    buffer->owner_queue = owner_queue;
}

void RenderGraph::AddSetupPass(StringView name, SetupExecute&& setup) {
    assert(!m_compiled);
    ValidateBuildThread();
    RGSetupPass pass{};
    pass.name    = String(name);
    pass.mode    = RGSetupPass::EMode::Lambda;
    pass.execute = [setup = std::move(setup)](RHICommandList* cmd_list, RGSetupContext& context) mutable {
        assert(cmd_list == nullptr);
        setup(context);
    };
    m_setup_passes.push_back(std::move(pass));
}

void RenderGraph::AddSetupPass(StringView name, Render::EQueueType queue, SetupCommandExecute&& setup) {
    assert(!m_compiled);
    ValidateBuildThread();
    assert(queue == Render::EQueueType::Graphics || queue == Render::EQueueType::Compute);
    RGSetupPass pass{};
    pass.name    = String(name);
    pass.queue   = queue;
    pass.mode    = RGSetupPass::EMode::CommandList;
    pass.execute = [setup = std::move(setup)](RHICommandList* cmd_list, RGSetupContext& context) mutable {
        assert(cmd_list != nullptr);
        setup(*cmd_list, context);
    };
    m_setup_passes.push_back(std::move(pass));
}

uint32_t RenderGraph::AddPassInternal(
    String                         name,
    void*                          parameters,
    std::type_index                type,
    uint32_t                       size,
    ERGPassFlags                   flags,
    bool                           serial,
    bool                           main_thread,
    Moer::Array<RGTextureAccess>&& texture_accesses,
    Moer::Array<RGBufferAccess>&&  buffer_accesses,
    RGPass::Execute&&              execute
) {
    assert(!m_compiled);
    assert(RGPassHasValidQueueFlags(flags));
    assert(main_thread ? RGPassHasValidQueueFlags(flags) : RGPassHasQueue(flags));
    assert(serial == RGPassIsSerial(flags));
    assert(!(main_thread && serial));
    ValidateBuildThread();
    const uint32_t pass_index = static_cast<uint32_t>(m_passes.size());
    auto&          pass       = m_passes.emplace_back();
    pass.name                 = std::move(name);
    pass.parameters           = parameters;
    pass.parameter_type       = type;
    pass.parameter_size       = size;
    pass.flags                = flags;
    pass.serial               = serial;
    pass.main_thread          = main_thread;
    pass.event_scopes         = m_scope_stack;
    pass.execute              = std::move(execute);
    pass.texture_accesses     = std::move(texture_accesses);
    pass.buffer_accesses      = std::move(buffer_accesses);
    for (const RGTextureAccess& access : pass.texture_accesses) {
        assert(access.texture != nullptr);
        assert(access.texture->kind == ERGResourceKind::Texture);
        assert(access.state != Render::ETextureState::UNDEFINED);
        assert(access.queue != Render::EQueueType::Ignore);
    }
    for (const RGBufferAccess& access : pass.buffer_accesses) {
        assert(access.buffer != nullptr);
        assert(access.buffer->kind == ERGResourceKind::Buffer);
        assert(access.state != Render::EBufferState::UNDEFINED);
        assert(access.queue != Render::EQueueType::Ignore);
    }
    return pass_index;
}

void RenderGraph::Compile(RGTransientResourceAllocator& allocator) {
    TRACE_SCOPE_CAT("RenderGraph.Compile", "RenderGraph");
    if (m_compiled) {
        return;
    }
    assert(m_scope_stack.empty() && "RenderGraph event scopes must end before dispatch");
    GraphEventRef setup_complete{};
    {
        TRACE_SCOPE_CAT("RenderGraph.Compile.DispatchSetupPasses", "RenderGraph");
        setup_complete = RunSetupPassesAsync();
    }
    {
        TRACE_SCOPE_CAT("RenderGraph.Compile.ValidateSetup", "RenderGraph");
        ValidateSetup();
    }
    {
        TRACE_SCOPE_CAT("RenderGraph.Compile.BuildAllocationStateRanges", "RenderGraph");
        BuildResourceStateRanges();
    }
    {
        TRACE_SCOPE_CAT("RenderGraph.Compile.Allocate", "RenderGraph");
        allocator.Allocate(*this);
    }
    {
        TRACE_SCOPE_CAT("RenderGraph.Compile.BuildStateRanges", "RenderGraph");
        BuildResourceStateRanges();
    }
    {
        TRACE_SCOPE_CAT("RenderGraph.Compile.BuildMetadata", "RenderGraph");
        BuildCompileMetadata();
    }
    {
        TRACE_SCOPE_CAT("RenderGraph.Compile.BuildExecutionBatches", "RenderGraph");
        BuildExecutionBatches();
    }
    if (setup_complete && !setup_complete->IsComplete()) {
        TRACE_SCOPE_CAT("RenderGraph.Compile.WaitSetupPasses", "RenderGraph");
        setup_complete->Wait(EThread::UNKNOWN_THREAD);
    }
    m_compiled = true;
}

void RenderGraph::Dispatch(RHICommandList* cmd_list) {
    Dispatch(RGTransientResourceAllocator::Global(), cmd_list);
}

void RenderGraph::Dispatch(RGTransientResourceAllocator& allocator, RHICommandList* cmd_list) {
    TRACE_SCOPE_CAT("RenderGraph.Dispatch", "RenderGraph");
    Compile(allocator);
    if (cmd_list == nullptr) {
        TRACE_SCOPE_CAT("RenderGraph.Dispatch.Batched", "RenderGraph");
        DispatchBatched();
        {
            TRACE_SCOPE_CAT("RenderGraph.Dispatch.Release", "RenderGraph");
            allocator.Release(*this);
        }
        return;
    }

    RGContext context(*this);
    TRACE_SCOPE_CAT("RenderGraph.Dispatch.ExternalRecord", "RenderGraph");
    Moer::Array<String> open_scopes{};
    SyncRGProfilerScopes(*cmd_list, open_scopes, m_name, {});
    for (auto& pass : m_passes) {
        if (pass.main_thread) {
            SyncRGProfilerScopes(*cmd_list, open_scopes, m_name, {});
            TRACE_SCOPE_CAT("RenderGraph.Dispatch.MainThreadPass", "RenderGraph");
            pass.execute(nullptr, context);
            continue;
        }
        if (pass.serial) {
            SyncRGProfilerScopes(*cmd_list, open_scopes, m_name, {});
            Render::CommandList serial_cmd_list(RGPassQueue(pass.flags));
            EmitPassTransitions(serial_cmd_list, pass);
            if (!pass.name.empty()) {
                serial_cmd_list.PushScopeWithTimeScope(pass.name);
            }
            pass.execute(&serial_cmd_list, context);
            if (!pass.name.empty()) {
                serial_cmd_list.PopScopeWithTimeScope();
            }
            if (!serial_cmd_list.IsEmpty()) {
                assert(false && "Serial RenderGraph pass recorded commands during external command-list dispatch");
            }
            continue;
        }
        SyncRGProfilerScopes(*cmd_list, open_scopes, m_name, pass.event_scopes);
        EmitPassTransitions(*cmd_list, pass);
        if (!pass.name.empty()) {
            cmd_list->PushScopeWithTimeScope(pass.name);
        }
        pass.execute(cmd_list, context);
        if (!pass.name.empty()) {
            cmd_list->PopScopeWithTimeScope();
        }
    }

    SyncRGProfilerScopes(*cmd_list, open_scopes, m_name, {});
    EmitFinalTrackedStates(*cmd_list);
    CloseRGProfilerScopes(*cmd_list, open_scopes);
    Moer::Array<PooledTextureRef> keep_alive_textures{};
    Moer::Array<PooledBufferRef>  keep_alive_buffers{};
    for (const Moer::UniquePtr<RGTexture>& texture : m_textures) {
        if (texture->transient.enabled && texture->Pooled()) {
            keep_alive_textures.push_back(texture->Pooled());
        }
    }
    for (const Moer::UniquePtr<RGBuffer>& buffer : m_buffers) {
        if (buffer->transient.enabled && buffer->Pooled()) {
            keep_alive_buffers.push_back(buffer->Pooled());
        }
    }
    if (!keep_alive_textures.empty() || !keep_alive_buffers.empty()) {
        cmd_list->AddCallback(
            [textures = std::move(keep_alive_textures), buffers = std::move(keep_alive_buffers)]() {
                (void)textures;
                (void)buffers;
            }
        );
    }
    allocator.Release(*this);
}

void RenderGraph::EmitFinalTrackedStates(RHICommandList& cmd_list) const {
    Moer::Array<Render::TrackedTextureState> textures{};
    Moer::Array<Render::TrackedBufferState>  buffers{};

    for (const Moer::UniquePtr<RGTexture>& texture : m_textures) {
        if (!texture->IsAllocated()) {
            continue;
        }
        auto* rhi_texture = texture->RHI().Get();
        if (rhi_texture == nullptr) {
            continue;
        }

        // Determine the effective final state: use exported final_state if set,
        // otherwise derive from the last state_range, or fall back to the texture's
        // implicit preferred layout as a SHADER_RESOURCE / UNORDERED_ACCESS state.
        Render::ETextureState effective_final_state = texture->final_state;
        if (effective_final_state == Render::ETextureState::UNDEFINED && !texture->state_ranges.empty()) {
            effective_final_state = texture->state_ranges.back().state;
        }
        if (effective_final_state == Render::ETextureState::UNDEFINED) {
            continue; // No known final state — skip (PersistentState stays as-initialized)
        }

        const EPassType final_pass_type = PassTypeForQueue(texture->owner_queue);
        bool            final_transition_emitted = false;
        if (texture->exported) {
            for (const RGTextureStateRange& state_range : texture->state_ranges) {
                const Render::EQueueType src_queue =
                    state_range.queue == Render::EQueueType::Ignore ? texture->owner_queue : state_range.queue;
                if (state_range.state == Render::ETextureState::UNDEFINED ||
                    (state_range.state == texture->final_state && src_queue == texture->owner_queue)) {
                    continue;
                }
                const std::array<Render::BarrierCreateInfo, 1> barriers{
                    Render::BarrierCreateInfo::Transition(
                        texture->GetView(state_range.range),
                        Render::MakeBarrierState(state_range.state, final_pass_type),
                        Render::MakeBarrierState(texture->final_state, final_pass_type)
                    )
                };
                cmd_list.Barriers(barriers, src_queue, texture->owner_queue, Render::ETrackedStateUpdateMode::Skip);
                final_transition_emitted = true;
            }
        }
        textures.emplace_back(
            Render::TrackedTextureState{
                .texture = Render::TextureView(rhi_texture, rhi_texture->GetFormat(), 0,
                    static_cast<uint8>(rhi_texture->GetNumMips())),
                .state        = effective_final_state,
                .owner_queue  = texture->owner_queue,
                .access_write = final_transition_emitted || RGTextureStateWrites(effective_final_state)
            }
        );
    }

    for (const Moer::UniquePtr<RGBuffer>& buffer : m_buffers) {
        if (!buffer->IsAllocated()) {
            continue;
        }
        auto* rhi_buffer = buffer->RHI().Get();
        if (rhi_buffer == nullptr) {
            continue;
        }

        Render::EBufferState effective_final_state = buffer->final_state;
        if (effective_final_state == Render::EBufferState::UNDEFINED && !buffer->state_ranges.empty()) {
            effective_final_state = buffer->state_ranges.back().state;
        }
        if (effective_final_state == Render::EBufferState::UNDEFINED) {
            continue;
        }

        const EPassType final_pass_type = PassTypeForQueue(buffer->owner_queue);
        if (buffer->exported) {
            for (const RGBufferStateRange& state_range : buffer->state_ranges) {
                const Render::EQueueType src_queue =
                    state_range.queue == Render::EQueueType::Ignore ? buffer->owner_queue : state_range.queue;
                if (state_range.state == Render::EBufferState::UNDEFINED ||
                    (state_range.state == buffer->final_state && src_queue == buffer->owner_queue)) {
                    continue;
                }
                const std::array<Render::BarrierCreateInfo, 1> barriers{
                    Render::BarrierCreateInfo::Transition(
                        buffer->GetView(state_range.range),
                        Render::MakeBarrierState(state_range.state, final_pass_type),
                        Render::MakeBarrierState(buffer->final_state, final_pass_type)
                    )
                };
                cmd_list.Barriers(barriers, src_queue, buffer->owner_queue, Render::ETrackedStateUpdateMode::Skip);
            }
        }
        buffers.emplace_back(
            Render::TrackedBufferState{
                .buffer       = rhi_buffer->GetView(),
                .state        = effective_final_state,
                .owner_queue  = buffer->owner_queue,
                .access_write = RGBufferStateWrites(effective_final_state)
            }
        );
    }

    if (!textures.empty() || !buffers.empty()) {
        cmd_list.SetTrackedState(std::move(textures), std::move(buffers));
    }
}

void RenderGraph::EmitPassTransitions(RHICommandList& cmd_list, const RGPass& pass) const {
    EmitRGPassTransitions(cmd_list, pass);
}

SharedPtr<RenderGraph::RGExecutionState> RenderGraph::DetachExecutionState() {
    auto state = MakeShared<RGExecutionState>();
    state->graph = MakeUnique<RenderGraph>(m_name);
    state->graph->m_textures = std::move(m_textures);
    state->graph->m_buffers = std::move(m_buffers);
    state->graph->m_resources = std::move(m_resources);
    state->graph->m_passes = m_passes;
    state->graph->m_allocations = std::move(m_allocations);
    state->graph->m_compiled_plan = m_compiled_plan;
    state->graph->m_setup_executed = m_setup_executed;
    state->graph->m_compiled = m_compiled;
    state->graph->m_scope_stack = m_scope_stack;
    state->resources = state->graph->m_resources;
    state->passes = state->graph->m_passes;
    state->compiled_plan = state->graph->m_compiled_plan;
    return state;
}

void RenderGraph::Reset() {
    assert(m_scope_stack.empty() && "RenderGraph destroyed with active event scopes");
    for (auto& allocation : m_allocations) {
        if (allocation.ptr && allocation.destroy) {
            allocation.destroy(allocation.ptr);
        }
    }
    m_allocations.clear();
    for (Moer::UniquePtr<RGTexture>& texture : m_textures) {
        texture->ReleaseTransient();
    }
    for (Moer::UniquePtr<RGBuffer>& buffer : m_buffers) {
        buffer->ReleaseTransient();
    }
    m_textures.clear();
    m_buffers.clear();
    m_resources.clear();
    m_setup_passes.clear();
    m_passes.clear();
    m_scope_stack.clear();
    m_compiled_plan.hazard_edges.clear();
    m_compiled_plan.execution_batches.clear();
    m_setup_executed = false;
    m_compiled       = false;
}

GraphEventRef RenderGraph::RunSetupPassesAsync() {
    TRACE_SCOPE_CAT("RenderGraph.RunSetupPassesAsync", "RenderGraph");
    if (m_setup_executed) {
        return nullptr;
    }

    GraphEventRef setup_complete = GraphEvent::CreateGraphEvent();
    LambdaTask::Create(
        [this]() mutable {
            TRACE_SCOPE_CAT("RenderGraph.RunSetupPasses", "RenderGraph");
            RGSetupContext setup_context(*this);
            Moer::Array<Render::CommandList> setup_command_lists{};
            for (auto& setup_pass : m_setup_passes) {
                if (!setup_pass.execute) {
                    continue;
                }
                if (setup_pass.mode == RGSetupPass::EMode::CommandList) {
                    TRACE_SCOPE_CAT("RenderGraph.RecordSetupPass", "RenderGraph");
                    Render::CommandList setup_cmd_list(setup_pass.queue);
                    if (!setup_pass.name.empty()) {
                        setup_cmd_list.PushScopeWithTimeScope(setup_pass.name);
                    }
                    setup_pass.execute(&setup_cmd_list, setup_context);
                    if (!setup_pass.name.empty()) {
                        setup_cmd_list.PopScopeWithTimeScope();
                    }
                    if (!setup_cmd_list.IsEmpty()) {
                        setup_command_lists.emplace_back(std::move(setup_cmd_list));
                    }
                } else {
                    TRACE_SCOPE_CAT("RenderGraph.SetupLambda", "RenderGraph");
                    setup_pass.execute(nullptr, setup_context);
                }
            }
            if (!setup_command_lists.empty()) {
                Render::RHIExecutor::Get().Submit(std::move(setup_command_lists), Render::ERHIExecSubmitFlags::None);
            }
        },
        EThread::AnyThread_NormalPri
    ).Next(setup_complete).Dispatch();
    m_setup_executed = true;
    return setup_complete;
}

const RGTexture& RenderGraph::GetTexture(const RGTexture* texture) const {
    assert(texture != nullptr);
    assert(texture->Index() < m_resources.size());
    assert(m_resources[texture->Index()] == texture);
    assert(texture->IsAllocated());
    return *texture;
}

const RGBuffer& RenderGraph::GetBuffer(const RGBuffer* buffer) const {
    assert(buffer != nullptr);
    assert(buffer->Index() < m_resources.size());
    assert(m_resources[buffer->Index()] == buffer);
    assert(buffer->IsAllocated());
    return *buffer;
}

RGTexture* RenderGraph::AddTexture(Moer::UniquePtr<RGTexture> texture) {
    assert(texture);
    for (const RGResource* existing : m_resources) {
        assert(existing->name != texture->name && "RenderGraph resource names must be unique");
    }
    RGTexture* resource = texture.get();
    resource->index     = static_cast<uint32_t>(m_resources.size());
    m_resources.push_back(resource);
    m_textures.push_back(std::move(texture));
    return resource;
}

RGBuffer* RenderGraph::AddBuffer(Moer::UniquePtr<RGBuffer> buffer) {
    assert(buffer);
    for (const RGResource* existing : m_resources) {
        assert(existing->name != buffer->name && "RenderGraph resource names must be unique");
    }
    RGBuffer* resource = buffer.get();
    resource->index    = static_cast<uint32_t>(m_resources.size());
    m_resources.push_back(resource);
    m_buffers.push_back(std::move(buffer));
    return resource;
}

void RenderGraph::ValidateSetup() const {
    Moer::Array<bool> has_texture_content_before_pass(m_resources.size(), false);
    Moer::Array<bool> has_buffer_content_before_pass(m_resources.size(), false);

    for (const auto& pass : m_passes) {
        assert(RGPassHasValidQueueFlags(pass.flags));
        if (pass.main_thread) {
            assert(pass.execute && "Main-thread RGPass must have one execution lambda");
            assert(!pass.serial);
            assert(pass.texture_accesses.empty() && pass.buffer_accesses.empty());
        } else if (pass.serial) {
            assert(pass.execute && "Serial RGPass must have one execution lambda");
            assert(RGPassHasQueue(pass.flags));
            assert(RGPassIsSerial(pass.flags));
        } else {
            assert(pass.execute && "Parallel RGPass must have one execution lambda");
            assert(RGPassHasQueue(pass.flags));
            assert(!RGPassIsSerial(pass.flags));
        }
        for (const auto& access : pass.texture_accesses) {
            const RGTexture& texture = *access.texture;
            if (!RGTextureStateInitializesContent(access.state) && !texture.imported &&
                !has_texture_content_before_pass[texture.Index()]) {
                MOER_ASSERT(
                    false,
                    "Graph-created texture resource {} read before any pass writes to it",
                    texture.Index()
                );
            }
            if (RGTextureStateInitializesContent(access.state)) {
                has_texture_content_before_pass[texture.Index()] = true;
            }
        }
        for (const auto& access : pass.buffer_accesses) {
            const RGBuffer& buffer = *access.buffer;
            if (!RGBufferStateInitializesContent(access.state) && !buffer.imported &&
                !has_buffer_content_before_pass[buffer.Index()]) {
                MOER_ASSERT(
                    false,
                    "Graph-created buffer resource {} read before any pass writes to it",
                    buffer.Index()
                );
            }
            if (RGBufferStateInitializesContent(access.state)) {
                has_buffer_content_before_pass[buffer.Index()] = true;
            }
        }
    }
}

void RenderGraph::ValidateBuildThread() const {
    assert(!IsGameThreadInitialized() || IsCurrentlyGameThread());
}

void RenderGraph::BuildResourceStateRanges() {
    for (Moer::UniquePtr<RGTexture>& texture : m_textures) {
        texture->state_ranges.clear();
        const RGTextureDesc& desc        = texture->Desc();
        const uint32_t       mip_count   = desc.num_mips == 0 ? 1u : static_cast<uint32_t>(desc.num_mips);
        const uint32_t       array_count = desc.array_size == 0 ? 1u : static_cast<uint32_t>(desc.array_size);
        Moer::Array<uint32_t> mip_boundaries{0, mip_count};
        Moer::Array<uint32_t> array_boundaries{0, array_count};

        if (texture->imported) {
            for (uint32_t mip = 1; mip < mip_count; ++mip) {
                AddBoundary(mip_boundaries, mip);
            }
            for (uint32_t array_index = 1; array_index < array_count; ++array_index) {
                AddBoundary(array_boundaries, array_index);
            }
        }

        for (const RGTextureStateRange& alias_range : texture->alias_initial_state_ranges) {
            AddBoundary(mip_boundaries, alias_range.range.mip_min);
            AddBoundary(mip_boundaries, alias_range.range.mip_min + alias_range.range.mip_count);
            AddBoundary(array_boundaries, alias_range.range.array_min);
            AddBoundary(array_boundaries, alias_range.range.array_min + alias_range.range.array_count);
        }

        for (const RGPass& pass : m_passes) {
            for (const RGTextureAccess& access : pass.texture_accesses) {
                if (access.texture != texture.get()) {
                    continue;
                }
                const RGTextureRange range = NormalizeTextureRange(*texture, access.range);
                AddBoundary(mip_boundaries, range.mip_min);
                AddBoundary(mip_boundaries, range.mip_min + range.mip_count);
                AddBoundary(array_boundaries, range.array_min);
                AddBoundary(array_boundaries, range.array_min + range.array_count);
            }
        }

        SortUnique(mip_boundaries);
        SortUnique(array_boundaries);
        const ETextureAspectFlags aspect_flags =
            desc.aspect_flags == ETextureAspectFlags::NONE ? ETextureAspectFlags::COLOR : desc.aspect_flags;
        for (ETextureAspectFlags aspect : TextureAspects(aspect_flags)) {
            for (uint32_t mip_index = 0; mip_index + 1 < mip_boundaries.size(); ++mip_index) {
                for (uint32_t array_index = 0; array_index + 1 < array_boundaries.size(); ++array_index) {
                    const uint32_t mip_begin   = mip_boundaries[mip_index];
                    const uint32_t mip_end     = mip_boundaries[mip_index + 1];
                    const uint32_t array_begin = array_boundaries[array_index];
                    const uint32_t array_end   = array_boundaries[array_index + 1];
                    if (mip_begin == mip_end || array_begin == array_end) {
                        continue;
                    }
                    texture->state_ranges.push_back(
                        RGTextureStateRange{
                            .range = RGTextureRange{
                                .aspect      = aspect,
                                .mip_min     = mip_begin,
                                .mip_count   = mip_end - mip_begin,
                                .array_min   = array_begin,
                                .array_count = array_end - array_begin
                            }
                        }
                    );
                }
            }
        }
        for (RGTextureStateRange& state_range : texture->state_ranges) {
            SeedImportedTextureStateRange(*texture, state_range);
        }
        if (!texture->imported) {
            SeedAliasTextureStateRanges(*texture);
        }
    }

    for (Moer::UniquePtr<RGBuffer>& buffer : m_buffers) {
        buffer->state_ranges.clear();
        const uint64_t byte_size = BufferByteSize(*buffer);
        Moer::Array<uint64_t> boundaries{0, byte_size};
        for (const RGPass& pass : m_passes) {
            for (const RGBufferAccess& access : pass.buffer_accesses) {
                if (access.buffer != buffer.get()) {
                    continue;
                }
                const RGBufferRange range = NormalizeBufferRange(*buffer, access.range);
                AddBoundary(boundaries, range.offset);
                AddBoundary(boundaries, range.offset + range.size);
            }
        }
        for (const RGBufferStateRange& alias_range : buffer->alias_initial_state_ranges) {
            AddBoundary(boundaries, alias_range.range.offset);
            AddBoundary(boundaries, alias_range.range.offset + alias_range.range.size);
        }

        SortUnique(boundaries);
        for (uint32_t boundary_index = 0; boundary_index + 1 < boundaries.size(); ++boundary_index) {
            const uint64_t begin = boundaries[boundary_index];
            const uint64_t end   = boundaries[boundary_index + 1];
            if (begin == end) {
                continue;
            }
            buffer->state_ranges.push_back(
                RGBufferStateRange{.range = RGBufferRange{.offset = begin, .size = end - begin}}
            );
        }
        SeedImportedBufferStateRanges(*buffer);
        if (!buffer->imported) {
            SeedAliasBufferStateRanges(*buffer);
        }
    }

}

void RenderGraph::BuildExecutionBatches() {
    m_compiled_plan.execution_batches.clear();
    static constexpr uint32_t target_batch_workload = 128;

    Moer::Array<uint32_t> pass_to_batch(m_passes.size(), invalid_pass);
    std::optional<RGCompiledExecutionBatch> active_batch{};

    auto flush_pending_command_list = [&]() {
        if (!active_batch.has_value() || active_batch->pass_count == 0) {
            active_batch.reset();
            return;
        }

        const uint32_t batch_index = static_cast<uint32_t>(m_compiled_plan.execution_batches.size());
        for (uint32_t offset = 0; offset < active_batch->pass_count; ++offset) {
            pass_to_batch[active_batch->first_pass + offset] = batch_index;
        }
        m_compiled_plan.execution_batches.push_back(std::move(*active_batch));
        active_batch.reset();
    };

    for (uint32_t pass_index = 0; pass_index < m_passes.size(); ++pass_index) {
        const RGPass& pass = m_passes[pass_index];
        if (pass.main_thread) {
            flush_pending_command_list();
            continue;
        }

        const uint32_t workload     = pass.workload == 0 ? 1 : pass.workload;
        const bool     async_record = !pass.serial;
        const Render::EQueueType queue = RGPassQueue(pass.flags);
        const bool starts_new_batch =
            !active_batch.has_value() ||
            active_batch->queue != queue ||
            active_batch->async_record != async_record ||
            (async_record && active_batch->workload + workload > target_batch_workload);

        if (starts_new_batch) {
            flush_pending_command_list();
            active_batch = RGCompiledExecutionBatch{
                .queue = queue,
                .first_pass = pass_index,
                .pass_count = 0,
                .workload = 0,
                .async_record = async_record
            };
        }

        ++active_batch->pass_count;
        active_batch->workload += workload;
    }
    flush_pending_command_list();

    for (const RGCompiledHazardEdge& edge : m_compiled_plan.hazard_edges) {
        if (!IsCrossQueueTransition(edge.src_queue, edge.dst_queue) ||
            edge.src_pass >= pass_to_batch.size() || edge.dst_pass >= pass_to_batch.size()) {
            continue;
        }

        const uint32_t src_batch = pass_to_batch[edge.src_pass];
        const uint32_t dst_batch = pass_to_batch[edge.dst_pass];
        if (src_batch == invalid_pass || dst_batch == invalid_pass || src_batch == dst_batch) {
            continue;
        }

        m_compiled_plan.execution_batches[src_batch].signal_sync_point = true;
        AppendUniqueIndex(m_compiled_plan.execution_batches[dst_batch].wait_sync_point_batches, src_batch);
    }
}

void RenderGraph::DispatchBatched() {
    TRACE_SCOPE_CAT("RenderGraph.DispatchBatched", "RenderGraph");
    SharedPtr<RGExecutionState> execution_state{};
    {
        TRACE_SCOPE_CAT("RenderGraph.DispatchBatched.DetachExecutionState", "RenderGraph");
        execution_state = DetachExecutionState();
    }
    RGContext context(*execution_state->graph);
    const Moer::Array<RGCompiledExecutionBatch>& execution_batches = execution_state->compiled_plan.execution_batches;
    Moer::Array<Render::SyncPointRef> signal_sync_point_by_batch(execution_batches.size());
    Moer::Array<SharedPtr<Render::CommandList>> pending_submit_command_lists{};
    Moer::Array<GraphEventRef> record_task_events{};
    Moer::Array<RGPreparedRecordedBatch> pending_serial_batches{};

    {
        TRACE_SCOPE_CAT("RenderGraph.DispatchBatched.CreateSyncPoints", "RenderGraph");
        for (uint32_t batch_index = 0; batch_index < execution_batches.size(); ++batch_index) {
            if (execution_batches[batch_index].signal_sync_point) {
                signal_sync_point_by_batch[batch_index] = Render::SyncPoint::Create(Render::ESyncPointMode::GPU);
            }
        }
    }

    auto submit_pending_command_lists = [&]() {
        TRACE_SCOPE_CAT("RenderGraph.SubmitPendingCommandLists", "RenderGraph");
        if (pending_submit_command_lists.empty()) {
            return;
        }
        Render::RHIExecutor::Get().SubmitRecording(
            std::move(pending_submit_command_lists),
            Render::ERHIExecSubmitFlags::None
        );
        pending_submit_command_lists.clear();
    };

    auto prepare_command_list = [&](uint32_t batch_index) {
        TRACE_SCOPE_CAT("RenderGraph.PrepareCommandList", "RenderGraph");
        RGPreparedRecordedBatch prepared_batch{};
        prepared_batch.batch_index = batch_index;
        prepared_batch.command_list = MakeShared<Render::CommandList>(execution_batches[batch_index].queue);
        prepared_batch.record_complete_event = GraphEvent::CreateGraphEvent();
        prepared_batch.command_list->SetRecordCompleteEvent(prepared_batch.record_complete_event);
        pending_submit_command_lists.push_back(prepared_batch.command_list);

        const RGCompiledExecutionBatch& batch = execution_batches[batch_index];
        if (!batch.async_record) {
            pending_serial_batches.push_back(std::move(prepared_batch));
            return;
        }

        LambdaTask::Create(
            [context,
             command_list = prepared_batch.command_list,
             batch_index,
             execution_state,
             signal_sync_point_by_batch]() mutable {
                TRACE_SCOPE_CAT("RenderGraph.RecordParallelBatch", "RenderGraph");
                const RGCompiledExecutionBatch& record_batch =
                    execution_state->compiled_plan.execution_batches[batch_index];
                {
                    TRACE_SCOPE_CAT("RenderGraph.RecordParallelBatch.PrepareCommandList", "RenderGraph");
                    PrepareRecordedBatchCommandList(
                        *command_list,
                        record_batch,
                        execution_state,
                        signal_sync_point_by_batch,
                        batch_index
                    );
                }
                {
                    TRACE_SCOPE_CAT("RenderGraph.RecordParallelBatch.Execute", "RenderGraph");
                    RecordExecutionBatch(
                        *command_list,
                        record_batch,
                        execution_state->passes,
                        execution_state->compiled_plan,
                        context,
                        execution_state->graph->Name()
                    );
                }
            },
            EThread::AnyThread_NormalPri
        ).Next(prepared_batch.record_complete_event).Dispatch();
        record_task_events.push_back(prepared_batch.record_complete_event);
    };

    auto complete_record_event = [&](const GraphEventRef& event) {
        if (!event) {
            return;
        }
        LambdaTask::Create([]() {}, EThread::AnyThread_NormalPri).Next(event).Dispatch();
        record_task_events.push_back(event);
    };

    auto record_pending_serial_batches = [&]() {
        TRACE_SCOPE_CAT("RenderGraph.RecordPendingSerialBatches", "RenderGraph");
        for (RGPreparedRecordedBatch& prepared_batch : pending_serial_batches) {
            const RGCompiledExecutionBatch& batch = execution_batches[prepared_batch.batch_index];
            {
                TRACE_SCOPE_CAT("RenderGraph.RecordSerialBatch.PrepareCommandList", "RenderGraph");
                PrepareRecordedBatchCommandList(
                    *prepared_batch.command_list,
                    batch,
                    execution_state,
                    signal_sync_point_by_batch,
                    prepared_batch.batch_index
                );
            }
            {
                TRACE_SCOPE_CAT("RenderGraph.RecordSerialBatch.Execute", "RenderGraph");
                RecordExecutionBatch(
                    *prepared_batch.command_list,
                    batch,
                    execution_state->passes,
                    execution_state->compiled_plan,
                    context,
                    execution_state->graph->Name()
                );
            }
            complete_record_event(prepared_batch.record_complete_event);
        }
        pending_serial_batches.clear();
    };

    uint32_t next_batch_index = 0;
    auto dispatch_batches_before_pass = [&](uint32_t pass_limit) {
        TRACE_SCOPE_CAT("RenderGraph.DispatchCommandBatchesBeforePass", "RenderGraph");
        while (next_batch_index < execution_batches.size() &&
               execution_batches[next_batch_index].first_pass < pass_limit) {
            prepare_command_list(next_batch_index);
            ++next_batch_index;
        }
        submit_pending_command_lists();
        record_pending_serial_batches();
    };

    for (uint32_t pass_index = 0; pass_index < execution_state->passes.size(); ++pass_index) {
        RGPass& pass = execution_state->passes[pass_index];
        if (!pass.main_thread) {
            continue;
        }
        dispatch_batches_before_pass(pass_index);
        TRACE_SCOPE_CAT("RenderGraph.MainThreadPass", "RenderGraph");
        pass.execute(nullptr, context);
    }
    dispatch_batches_before_pass(static_cast<uint32_t>(execution_state->passes.size()));

    {
        TRACE_SCOPE_CAT("RenderGraph.DispatchBatched.FinalTrackedStates", "RenderGraph");
        const Render::EQueueType final_queue = execution_batches.empty()
            ? Render::EQueueType::Graphics
            : execution_batches.back().queue;
        auto final_state_command_list = MakeShared<Render::CommandList>(final_queue);
        final_state_command_list->AddCallback([execution_state]() {});
        final_state_command_list->SetExplicitTrackedState({}, {});
        EmitFinalTrackedStates(*final_state_command_list);
        if (!final_state_command_list->IsEmpty()) {
            Moer::Array<SharedPtr<Render::CommandList>> final_command_lists{};
            final_command_lists.push_back(std::move(final_state_command_list));
            Render::RHIExecutor::Get().SubmitRecording(
                std::move(final_command_lists), Render::ERHIExecSubmitFlags::None
            );
        }
    }

    {
        TRACE_SCOPE_CAT("RenderGraph.DispatchBatched.PublishEvents", "RenderGraph");
        PublishLastRenderGraphEvents(std::move(record_task_events));
    }
}

void RenderGraph::BuildCompileMetadata() {
    m_compiled_plan.hazard_edges.clear();
    m_compiled_plan.execution_batches.clear();
    for (RGResource* resource : m_resources) {
        resource->compile.Reset();
        resource->transient.ResetCompileState();
    }
    for (RGPass& pass : m_passes) {
        pass.compile.Reset();
    }

    Moer::UnorderedMap<uint64_t, uint32_t> hazard_indices{};

    const auto add_dependency = [this, &hazard_indices](
                                    uint32_t              src,
                                    uint32_t              dst,
                                    RGResource&           resource,
                                    ERGCompiledHazardFlag flag,
                                    Render::EQueueType    src_queue,
                                    Render::EQueueType    dst_queue
                                ) {
        if (src == dst || src == invalid_pass || dst == invalid_pass) {
            return;
        }

        RGPass& src_pass = m_passes[src];
        RGPass& dst_pass = m_passes[dst];
        RecordLatestPass(dst_pass.compile.last_pass, src);
        if (IsValidQueue(src_queue)) {
            RecordLatestPass(dst_pass.compile.last_pass_by_queue[QueueIndex(src_queue)], src);
        }
        if (IsValidQueue(dst_queue)) {
            RecordEarliestPass(src_pass.compile.next_pass_by_queue[QueueIndex(dst_queue)], dst);
        }

        const uint64_t key      = MakeHazardKey(src, dst, resource);
        const auto     existing = hazard_indices.find(key);
        if (existing != hazard_indices.end()) {
            RGCompiledHazardEdge& edge = m_compiled_plan.hazard_edges[existing->second];
            edge.flags |= RGCompiledHazardFlagMask(flag);
            if (IsValidQueue(src_queue)) {
                edge.src_queue = src_queue;
            }
            if (IsValidQueue(dst_queue)) {
                edge.dst_queue = dst_queue;
            }
            return;
        }
        hazard_indices.emplace(key, static_cast<uint32_t>(m_compiled_plan.hazard_edges.size()));
        m_compiled_plan.hazard_edges.push_back(
            RGCompiledHazardEdge{
                .src_pass      = src,
                .dst_pass      = dst,
                .resource      = &resource,
                .resource_kind = resource.kind,
                .flags         = RGCompiledHazardFlagMask(flag),
                .src_queue     = src_queue,
                .dst_queue     = dst_queue
            }
        );
    };

    auto process_texture_access = [&](uint32_t pass_index, const RGTextureAccess& access) {
        RGTexture& texture = *access.texture;
        texture.compile.RecordAccess(pass_index);
        const RGTextureRange access_range = NormalizeTextureRange(texture, access.range);

        for (RGTextureStateRange& state_range : texture.state_ranges) {
            if (!state_range.range.Overlaps(access_range)) {
                continue;
            }
            RGPass& pass = m_passes[pass_index];
            pass.compile.texture_transitions.emplace_back(RGCompiledTextureTransition{
                .texture = &texture,
                .range = state_range.range,
                .src_state = state_range.state,
                .state = access.state,
                .src_queue = state_range.queue == Render::EQueueType::Ignore ? access.queue : state_range.queue,
                .dst_queue = access.queue
            });
            if (state_range.last_pass != invalid_pass && state_range.alias_initial) {
                add_dependency(
                    state_range.last_pass,
                    pass_index,
                    texture,
                    ERGCompiledHazardFlag::AliasReuse,
                    state_range.queue,
                    access.queue
                );
            }
            if (state_range.last_pass != invalid_pass && !RGTextureStatesMergeable(state_range.state, access.state)) {
                add_dependency(
                    state_range.last_pass,
                    pass_index,
                    texture,
                    ERGCompiledHazardFlag::AccessConflict,
                    state_range.queue,
                    access.queue
                );
            }
            if (texture.transient.enabled && state_range.last_pass != invalid_pass &&
                state_range.queue != access.queue) {
                add_dependency(
                    state_range.last_pass,
                    pass_index,
                    texture,
                    ERGCompiledHazardFlag::OwnerTransfer,
                    state_range.queue,
                    access.queue
                );
            }

            state_range.state      = access.state;
            state_range.queue      = access.queue;
            state_range.last_pass  = pass_index;
            state_range.alias_initial = false;
        }
        if (texture.transient.enabled) {
            texture.transient.SetOwner(access.queue, pass_index);
        }
    };

    auto process_buffer_access = [&](uint32_t pass_index, const RGBufferAccess& access) {
        RGBuffer& buffer = *access.buffer;
        buffer.compile.RecordAccess(pass_index);
        const RGBufferRange access_range = NormalizeBufferRange(buffer, access.range);

        for (RGBufferStateRange& state_range : buffer.state_ranges) {
            if (!state_range.range.Overlaps(access_range)) {
                continue;
            }
            RGPass& pass = m_passes[pass_index];
            pass.compile.buffer_transitions.emplace_back(RGCompiledBufferTransition{
                .buffer = &buffer,
                .range = state_range.range,
                .src_state = state_range.state,
                .state = access.state,
                .src_queue = state_range.queue == Render::EQueueType::Ignore ? access.queue : state_range.queue,
                .dst_queue = access.queue
            });
            if (state_range.last_pass != invalid_pass && state_range.alias_initial) {
                add_dependency(
                    state_range.last_pass,
                    pass_index,
                    buffer,
                    ERGCompiledHazardFlag::AliasReuse,
                    state_range.queue,
                    access.queue
                );
            }
            if (state_range.last_pass != invalid_pass && !RGBufferStatesMergeable(state_range.state, access.state)) {
                add_dependency(
                    state_range.last_pass,
                    pass_index,
                    buffer,
                    ERGCompiledHazardFlag::AccessConflict,
                    state_range.queue,
                    access.queue
                );
            }
            if (buffer.transient.enabled && state_range.last_pass != invalid_pass &&
                state_range.queue != access.queue) {
                add_dependency(
                    state_range.last_pass,
                    pass_index,
                    buffer,
                    ERGCompiledHazardFlag::OwnerTransfer,
                    state_range.queue,
                    access.queue
                );
            }

            state_range.state      = access.state;
            state_range.queue      = access.queue;
            state_range.last_pass  = pass_index;
            state_range.alias_initial = false;
        }
        if (buffer.transient.enabled) {
            buffer.transient.SetOwner(access.queue, pass_index);
        }
    };

    for (uint32_t pass_index = 0; pass_index < m_passes.size(); ++pass_index) {
        RGPass& pass = m_passes[pass_index];
        if (pass.main_thread) {
            continue;
        }
        for (const RGTextureAccess& access : pass.texture_accesses) {
            process_texture_access(pass_index, access);
        }
        for (const RGBufferAccess& access : pass.buffer_accesses) {
            process_buffer_access(pass_index, access);
        }
    }
}

} // namespace Moer::Render
