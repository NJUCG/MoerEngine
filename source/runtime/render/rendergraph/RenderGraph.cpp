#include "rendergraph/RenderGraph.h"

#include "misc/Assert.h"
#include "rhi/RHICommand.h"
#include "taskgraph/TaskGraph.h"

#include <optional>
#include <utility>

namespace Moer::Render {

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
    if (EnumHasAnyFlag(flags, ERGPassFlags::Compute)) {
        return EPassType::Compute;
    }
    if (EnumHasAnyFlag(flags, ERGPassFlags::Copy)) {
        return EPassType::Copy;
    }
    return EPassType::Graphics;
}

RenderGraph::RenderGraph() :
    m_texture_pool(&PooledTexturePool::Global()),
    m_buffer_pool(&PooledBufferPool::Global()) {}

RenderGraph::RenderGraph(PooledTexturePool& texture_pool, PooledBufferPool& buffer_pool) :
    m_texture_pool(&texture_pool),
    m_buffer_pool(&buffer_pool) {}

RenderGraph::~RenderGraph() {
    Reset();
}

RenderGraphHandle RenderGraph::CreateTexture(StringView name, const RGTextureDesc& desc) {
    assert(m_phase == ERGPhase::Setup);
    RGResource resource{};
    resource.name              = String(name);
    resource.kind              = ERGResourceKind::Texture;
    resource.transient.enabled = true;
    resource.texture_desc      = desc;
    return AddResource(std::move(resource));
}

RenderGraphHandle RenderGraph::CreateBuffer(StringView name, const RGBufferDesc& desc) {
    assert(m_phase == ERGPhase::Setup);
    RGResource resource{};
    resource.name              = String(name);
    resource.kind              = ERGResourceKind::Buffer;
    resource.transient.enabled = true;
    resource.buffer_desc       = desc;
    return AddResource(std::move(resource));
}

RenderGraphHandle
RenderGraph::RegisterTexture(StringView name, PooledTextureRef texture, Render::EQueueType owner_queue) {
    assert(m_phase == ERGPhase::Setup);
    assert(texture && texture->IsAllocated());
    RGResource resource{};
    resource.name                   = String(name);
    resource.kind                   = ERGResourceKind::Texture;
    resource.imported               = true;
    resource.texture_desc           = texture->Desc();
    resource.owner_queue            = owner_queue;
    const RenderGraphHandle handle  = AddResource(std::move(resource));
    CheckedResource(handle).texture = MakeShared<RGTexture>(name, std::move(texture), true);
    return handle;
}

RenderGraphHandle
RenderGraph::RegisterBuffer(StringView name, PooledBufferRef buffer, Render::EQueueType owner_queue) {
    assert(m_phase == ERGPhase::Setup);
    assert(buffer && buffer->IsAllocated());
    RGResource resource{};
    resource.name                  = String(name);
    resource.kind                  = ERGResourceKind::Buffer;
    resource.imported              = true;
    resource.buffer_desc           = buffer->Desc();
    resource.owner_queue           = owner_queue;
    const RenderGraphHandle handle = AddResource(std::move(resource));
    CheckedResource(handle).buffer = MakeShared<RGBuffer>(name, std::move(buffer), true);
    return handle;
}

RenderGraphHandle
RenderGraph::ImportTexture(StringView name, Render::TextureRef texture, Render::EQueueType owner_queue) {
    assert(m_phase == ERGPhase::Setup);
    assert(texture);
    return RegisterTexture(name, m_texture_pool->RegisterExternal(name, std::move(texture)), owner_queue);
}

RenderGraphHandle
RenderGraph::ImportBuffer(StringView name, Render::BufferRef buffer, Render::EQueueType owner_queue) {
    assert(m_phase == ERGPhase::Setup);
    assert(buffer);
    return RegisterBuffer(name, m_buffer_pool->RegisterExternal(name, std::move(buffer)), owner_queue);
}

void RenderGraph::ExportTexture(
    RenderGraphHandle     handle,
    Render::ETextureState final_state,
    Render::EQueueType    owner_queue
) {
    auto& resource = CheckedResource(handle);
    assert(resource.kind == ERGResourceKind::Texture);
    assert(final_state != Render::ETextureState::UNDEFINED);
    resource.exported            = true;
    resource.final_texture_state = final_state;
    resource.owner_queue         = owner_queue;
}

void RenderGraph::ExportBuffer(
    RenderGraphHandle    handle,
    Render::EBufferState final_state,
    Render::EQueueType   owner_queue
) {
    auto& resource = CheckedResource(handle);
    assert(resource.kind == ERGResourceKind::Buffer);
    assert(final_state != Render::EBufferState::UNDEFINED);
    resource.exported           = true;
    resource.final_buffer_state = final_state;
    resource.owner_queue        = owner_queue;
}

void RenderGraph::AddSetupPass(StringView name, SetupExecute&& setup) {
    assert(m_phase == ERGPhase::Setup);
    m_setup_passes.push_back(RGSetupPass{String(name), std::move(setup)});
}

uint32_t RenderGraph::AddPassInternal(
    String                    name,
    void*                     parameters,
    std::type_index           type,
    uint32_t                  size,
    ERGPassFlags              flags,
    ERGPassExecutionMode      execution_mode,
    Moer::Array<RGTextureAccess>&& texture_accesses,
    Moer::Array<RGBufferAccess>&&  buffer_accesses,
    RGPass::ParallelExecute&& parallel_execute,
    RGPass::SerialExecute&&   serial_execute
) {
    assert(m_phase == ERGPhase::Setup);
    assert(RGPassHasValidQueueFlags(flags));
    assert(
        (execution_mode == ERGPassExecutionMode::Parallel && RGPassHasQueue(flags)) ||
        (execution_mode == ERGPassExecutionMode::Serial && flags == ERGPassFlags::None)
    );
    const uint32_t pass_index = static_cast<uint32_t>(m_passes.size());
    auto&          pass       = m_passes.emplace_back();
    pass.name                 = std::move(name);
    pass.parameters           = parameters;
    pass.parameter_type       = type;
    pass.parameter_size       = size;
    pass.flags                = flags;
    pass.execution_mode       = execution_mode;
    pass.parallel_execute     = std::move(parallel_execute);
    pass.serial_execute       = std::move(serial_execute);
    pass.texture_accesses     = std::move(texture_accesses);
    pass.buffer_accesses      = std::move(buffer_accesses);
    for (const RGTextureAccess& access : pass.texture_accesses) {
        const auto& resource = CheckedResource(access.handle);
        assert(resource.kind == ERGResourceKind::Texture);
        assert(access.state != Render::ETextureState::UNDEFINED);
        assert(access.queue != Render::EQueueType::Ignore);
    }
    for (const RGBufferAccess& access : pass.buffer_accesses) {
        const auto& resource = CheckedResource(access.handle);
        assert(resource.kind == ERGResourceKind::Buffer);
        assert(access.state != Render::EBufferState::UNDEFINED);
        assert(access.queue != Render::EQueueType::Ignore);
    }
    return pass_index;
}

void RenderGraph::Compile() {
    if (m_phase != ERGPhase::Setup) {
        return;
    }
    RunSetupPasses();
    ValidateSetup();
    AllocateTransientResources();
    BuildCompileMetadata();
    m_phase = ERGPhase::Compiled;
}

void RenderGraph::Dispatch(RHICommandList* cmd_list) {
    Compile();
    if (cmd_list == nullptr) {
        DispatchBatched();
        ReleaseTransientResources();
        m_phase = ERGPhase::Dispatched;
        return;
    }

    RGContext context(*this);
    for (auto& pass : m_passes) {
        if (pass.execution_mode == ERGPassExecutionMode::Serial) {
            pass.serial_execute(context);
            continue;
        }
        if (cmd_list) {
            pass.parallel_execute(*cmd_list, context);
        }
    }

    if (cmd_list != nullptr) {
        EmitFinalTrackedStates(*cmd_list);
    }

    ReleaseTransientResources();

    m_phase = ERGPhase::Dispatched;
}

void RenderGraph::EmitFinalTrackedStates(RHICommandList& cmd_list) const {
    Moer::Array<Render::TrackedTextureState> textures{};
    Moer::Array<Render::TrackedBufferState>  buffers{};

    for (uint32_t resource_index = 0; resource_index < m_resources.size(); ++resource_index) {
        const auto& resource = m_resources[resource_index];
        if (!resource.exported || !resource.imported) {
            continue;
        }

        const auto handle = RenderGraphHandle(static_cast<RenderGraphHandle::Index>(resource_index));
        if (resource.kind == ERGResourceKind::Texture) {
            if (!resource.texture) {
                continue;
            }
            auto* texture = resource.texture->RHI().Get();
            if (texture == nullptr) {
                continue;
            }
            textures.emplace_back(
                Render::TrackedTextureState{
                    .texture = Render::TextureView(
                        texture, texture->GetFormat(), 0, static_cast<uint8>(texture->GetNumMips())
                    ),
                    .state        = resource.final_texture_state,
                    .owner_queue  = resource.owner_queue,
                    .access_write = resource.compile.access_write
                }
            );
            continue;
        }

        if (!resource.buffer) {
            continue;
        }
        auto* buffer = resource.buffer->RHI().Get();
        if (buffer == nullptr) {
            continue;
        }
        buffers.emplace_back(
            Render::TrackedBufferState{
                .buffer       = buffer->GetView(),
                .state        = resource.final_buffer_state,
                .owner_queue  = resource.owner_queue,
                .access_write = resource.compile.access_write
            }
        );
    }

    if (!textures.empty() || !buffers.empty()) {
        cmd_list.SetTrackedState(std::move(textures), std::move(buffers));
    }
}

void RenderGraph::Reset() {
    for (auto& allocation : m_allocations) {
        if (allocation.ptr && allocation.destroy) {
            allocation.destroy(allocation.ptr);
        }
    }
    m_allocations.clear();
    ReleaseTransientResources();
    m_resources.clear();
    m_setup_passes.clear();
    m_passes.clear();
    m_compiled_plan.hazard_edges.clear();
    m_compiled_plan.execution_batches.clear();
    m_setup_executed = false;
    m_phase          = ERGPhase::Setup;
}

void RenderGraph::RunSetupPasses() {
    if (m_setup_executed) {
        return;
    }
    RGSetupContext setup_context(*this);
    for (auto& setup_pass : m_setup_passes) {
        if (setup_pass.execute) {
            setup_pass.execute(setup_context);
        }
    }
    m_setup_executed = true;
}

RGResource& RenderGraph::CheckedResource(RenderGraphHandle handle) {
    assert(handle.IsInitialized() && handle.index < m_resources.size());
    return m_resources[handle.index];
}

const RGResource& RenderGraph::CheckedResource(RenderGraphHandle handle) const {
    assert(handle.IsInitialized() && handle.index < m_resources.size());
    return m_resources[handle.index];
}

const RGTexture& RenderGraph::GetTexture(RenderGraphHandle handle) const {
    const RGResource& resource = CheckedResource(handle);
    assert(resource.kind == ERGResourceKind::Texture);
    assert(resource.texture && resource.texture->IsAllocated());
    return *resource.texture;
}

const RGBuffer& RenderGraph::GetBuffer(RenderGraphHandle handle) const {
    const RGResource& resource = CheckedResource(handle);
    assert(resource.kind == ERGResourceKind::Buffer);
    assert(resource.buffer && resource.buffer->IsAllocated());
    return *resource.buffer;
}

RenderGraphHandle RenderGraph::AddResource(RGResource&& resource) {
    assert(m_resources.size() < RenderGraphHandle::uninitialized);
    for (const auto& existing : m_resources) {
        assert(existing.name != resource.name && "RenderGraph resource names must be unique");
    }
    const auto handle = RenderGraphHandle(static_cast<RenderGraphHandle::Index>(m_resources.size()));
    m_resources.push_back(std::move(resource));
    return handle;
}

void RenderGraph::ValidateSetup() const {
    Moer::Array<bool> has_texture_write_before_pass(m_resources.size(), false);
    Moer::Array<bool> has_buffer_write_before_pass(m_resources.size(), false);

    for (const auto& pass : m_passes) {
        assert(RGPassHasValidQueueFlags(pass.flags));
        if (pass.execution_mode == ERGPassExecutionMode::Serial) {
            assert(pass.serial_execute && "Serial RGPass must have one serial execution lambda");
            assert(pass.flags == ERGPassFlags::None);
            assert(pass.texture_accesses.empty() && pass.buffer_accesses.empty());
        } else {
            assert(pass.parallel_execute && "Parallel RGPass must have one parallel execution lambda");
            assert(RGPassHasQueue(pass.flags));
        }
        for (const auto& access : pass.texture_accesses) {
            const auto& resource = CheckedResource(access.handle);
            assert(resource.kind == ERGResourceKind::Texture);
            if (!RGTextureStateWrites(access.state) && !resource.imported &&
                !has_texture_write_before_pass[access.handle.index]) {
                MOER_ASSERT(
                    false,
                    "Graph-created texture handle {} read before any pass writes to it",
                    access.handle.index
                );
            }
            if (RGTextureStateWrites(access.state)) {
                has_texture_write_before_pass[access.handle.index] = true;
            }
        }
        for (const auto& access : pass.buffer_accesses) {
            const auto& resource = CheckedResource(access.handle);
            assert(resource.kind == ERGResourceKind::Buffer);
            if (!RGBufferStateWrites(access.state) && !resource.imported &&
                !has_buffer_write_before_pass[access.handle.index]) {
                MOER_ASSERT(
                    false,
                    "Graph-created buffer handle {} read before any pass writes to it",
                    access.handle.index
                );
            }
            if (RGBufferStateWrites(access.state)) {
                has_buffer_write_before_pass[access.handle.index] = true;
            }
        }
    }
}

void RenderGraph::AllocateTransientResources() {
    assert(m_texture_pool != nullptr && m_buffer_pool != nullptr);
    Moer::Array<PooledTextureAllocationRequest> texture_requests{};
    Moer::Array<RenderGraphHandle>              texture_handles{};
    Moer::Array<PooledBufferAllocationRequest>  buffer_requests{};
    Moer::Array<RenderGraphHandle>              buffer_handles{};

    for (uint32_t resource_index = 0; resource_index < m_resources.size(); ++resource_index) {
        RGResource& resource = m_resources[resource_index];
        if (resource.imported) {
            continue;
        }

        const RenderGraphHandle handle(static_cast<RenderGraphHandle::Index>(resource_index));
        if (resource.kind == ERGResourceKind::Texture) {
            if (!resource.texture) {
                texture_requests.push_back(
                    PooledTextureAllocationRequest{resource.name, resource.texture_desc}
                );
                texture_handles.push_back(handle);
            }
            continue;
        }

        if (!resource.buffer) {
            buffer_requests.push_back(PooledBufferAllocationRequest{resource.name, resource.buffer_desc});
            buffer_handles.push_back(handle);
        }
    }

    Moer::Array<PooledTextureAllocationResult> texture_allocations =
        m_texture_pool->AllocateBatch(texture_requests);
    assert(texture_allocations.size() == texture_handles.size());
    for (uint32_t index = 0; index < texture_allocations.size(); ++index) {
        RGResource& resource = CheckedResource(texture_handles[index]);
        resource.texture =
            MakeShared<RGTexture>(resource.name, std::move(texture_allocations[index].texture), false);
    }

    Moer::Array<PooledBufferAllocationResult> buffer_allocations =
        m_buffer_pool->AllocateBatch(buffer_requests);
    assert(buffer_allocations.size() == buffer_handles.size());
    for (uint32_t index = 0; index < buffer_allocations.size(); ++index) {
        RGResource& resource = CheckedResource(buffer_handles[index]);
        resource.buffer =
            MakeShared<RGBuffer>(resource.name, std::move(buffer_allocations[index].buffer), false);
    }
}

void RenderGraph::ReleaseTransientResources() {
    for (RGResource& resource : m_resources) {
        if (resource.imported) {
            continue;
        }
        resource.texture.reset();
        resource.buffer.reset();
    }
}

void RenderGraph::DispatchBatched() {
    RGContext context(*this);
    GraphEventArray record_events{};
    GraphEventArray pending_before_serial{};
    m_compiled_plan.execution_batches.clear();
    static constexpr uint32_t target_batch_workload = 8;

    auto wait_pending_before_serial = [&pending_before_serial]() {
        if (pending_before_serial.empty()) {
            return;
        }
        TaskGraph::GetInterface().WaitUntilTasksComplete(pending_before_serial, EThread::UNKNOWN_THREAD);
        pending_before_serial.clear();
    };

    auto submit_batch = [this, context, &record_events, &pending_before_serial](
                            std::optional<RGCompiledExecutionBatch>& active_batch
                        ) mutable {
        if (!active_batch.has_value() || active_batch->pass_count == 0) {
            active_batch.reset();
            return;
        }

        const RGCompiledExecutionBatch batch = *active_batch;
        m_compiled_plan.execution_batches.push_back(batch);
        active_batch.reset();

        auto command_list = MakeShared<Render::CommandList>(batch.queue);
        GraphEventRef record_complete_event = GraphEvent::CreateGraphEvent();
        command_list->SetRecordCompleteEvent(record_complete_event);
        record_events.push_back(record_complete_event);
        pending_before_serial.push_back(record_complete_event);

        LambdaTask::Create(
            [this, context, command_list, batch]() mutable {
                for (uint32_t offset = 0; offset < batch.pass_count; ++offset) {
                    RGPass& batch_pass = m_passes[batch.first_pass + offset];
                    batch_pass.parallel_execute(*command_list, context);
                }
            },
            EThread::AnyThread_NormalPri
        ).Next(record_complete_event).Dispatch();

        Moer::Array<SharedPtr<Render::CommandList>> command_lists{};
        command_lists.push_back(command_list);
        Render::RHIExecutor::Get().SubmitRecording(std::move(command_lists), Render::ERHIExecSubmitFlags::None);
    };

    std::optional<RGCompiledExecutionBatch> active_batch{};
    for (uint32_t pass_index = 0; pass_index < m_passes.size(); ++pass_index) {
        RGPass& pass = m_passes[pass_index];
        if (pass.execution_mode == ERGPassExecutionMode::Serial) {
            submit_batch(active_batch);
            wait_pending_before_serial();
            pass.serial_execute(context);
            continue;
        }

        const Render::EQueueType queue    = RGPassQueue(pass.flags);
        const uint32_t           workload = pass.workload == 0 ? 1 : pass.workload;
        const bool starts_new_batch       = !active_batch.has_value() || active_batch->queue != queue ||
                                      active_batch->workload + workload > target_batch_workload;
        if (starts_new_batch) {
            submit_batch(active_batch);
            active_batch = RGCompiledExecutionBatch{
                .queue      = queue,
                .first_pass = pass_index,
                .pass_count = 0,
                .workload   = 0
            };
        }

        ++active_batch->pass_count;
        active_batch->workload += workload;
    }
    submit_batch(active_batch);

    if (!record_events.empty()) {
        TaskGraph::GetInterface().WaitUntilTasksComplete(record_events, EThread::UNKNOWN_THREAD);
    }

    Render::CommandList final_state_cmd_list(Render::EQueueType::Graphics);
    EmitFinalTrackedStates(final_state_cmd_list);
    if (!final_state_cmd_list.IsEmpty()) {
        Moer::Array<Render::CommandList> final_state_command_lists{};
        final_state_command_lists.emplace_back(std::move(final_state_cmd_list));
        Render::RHIExecutor::Get().Submit(std::move(final_state_command_lists), Render::ERHIExecSubmitFlags::None);
    }
}

void RenderGraph::BuildCompileMetadata() {
    m_compiled_plan.hazard_edges.clear();
    m_compiled_plan.execution_batches.clear();
    for (RGResource& resource : m_resources) {
        resource.compile.Reset();
        resource.transient.ResetCompileState();
    }
    for (RGPass& pass : m_passes) {
        pass.compile.Reset();
    }

    static constexpr uint32_t invalid_pass = RGPass::invalid_pass;

    struct TextureCompileState {
        RGTextureRange        range{};
        Render::ETextureState state{Render::ETextureState::UNDEFINED};
        Render::EQueueType    queue{Render::EQueueType::Ignore};
        uint32_t              pass{invalid_pass};
    };

    struct BufferCompileState {
        RGBufferRange        range{};
        Render::EBufferState state{Render::EBufferState::UNDEFINED};
        Render::EQueueType   queue{Render::EQueueType::Ignore};
        uint32_t             pass{invalid_pass};
    };

    Moer::Array<Moer::Array<TextureCompileState>> texture_states(m_resources.size());
    Moer::Array<Moer::Array<BufferCompileState>>  buffer_states(m_resources.size());
    Moer::UnorderedMap<uint64_t, uint32_t>         hazard_indices{};

    const auto is_valid_queue = [](Render::EQueueType queue) {
        return queue != Render::EQueueType::Ignore && queue != Render::EQueueType::Num;
    };

    const auto queue_index = [](Render::EQueueType queue) {
        assert(queue != Render::EQueueType::Ignore && queue != Render::EQueueType::Num);
        return static_cast<size_t>(queue);
    };

    const auto record_latest_pass = [](uint32_t& slot, uint32_t pass_index) {
        if (slot == invalid_pass || slot < pass_index) {
            slot = pass_index;
        }
    };

    const auto record_earliest_pass = [](uint32_t& slot, uint32_t pass_index) {
        if (slot == invalid_pass || pass_index < slot) {
            slot = pass_index;
        }
    };

    const auto make_hazard_key = [](uint32_t src_pass, uint32_t dst_pass, RenderGraphHandle resource, ERGResourceKind resource_kind) {
        assert(src_pass < (1u << 20));
        assert(dst_pass < (1u << 20));
        return (static_cast<uint64_t>(src_pass) << 37) | (static_cast<uint64_t>(dst_pass) << 17) |
               (static_cast<uint64_t>(resource_kind) << 16) | static_cast<uint64_t>(resource.index);
    };

    const auto add_dependency = [this,
                                 &hazard_indices,
                                 is_valid_queue,
                                 queue_index,
                                 record_latest_pass,
                                 record_earliest_pass,
                                 make_hazard_key](
                                uint32_t              src,
                                uint32_t              dst,
                                RenderGraphHandle     resource,
                                ERGResourceKind       resource_kind,
                                ERGCompiledHazardFlag flag,
                                Render::EQueueType    src_queue,
                                Render::EQueueType    dst_queue
                            ) {
        if (src == dst || src == invalid_pass || dst == invalid_pass) {
            return;
        }

        RGPass& src_pass = m_passes[src];
        RGPass& dst_pass = m_passes[dst];
        record_latest_pass(dst_pass.compile.last_pass, src);
        if (is_valid_queue(src_queue)) {
            record_latest_pass(dst_pass.compile.last_pass_by_queue[queue_index(src_queue)], src);
        }
        if (is_valid_queue(dst_queue)) {
            record_earliest_pass(src_pass.compile.next_pass_by_queue[queue_index(dst_queue)], dst);
        }

        const uint64_t key      = make_hazard_key(src, dst, resource, resource_kind);
        const auto     existing = hazard_indices.find(key);
        if (existing != hazard_indices.end()) {
            RGCompiledHazardEdge& edge = m_compiled_plan.hazard_edges[existing->second];
            edge.flags |= RGCompiledHazardFlagMask(flag);
            if (is_valid_queue(src_queue)) {
                edge.src_queue = src_queue;
            }
            if (is_valid_queue(dst_queue)) {
                edge.dst_queue = dst_queue;
            }
            return;
        }
        hazard_indices.emplace(key, static_cast<uint32_t>(m_compiled_plan.hazard_edges.size()));
        m_compiled_plan.hazard_edges.push_back(
            RGCompiledHazardEdge{
                .src_pass      = src,
                .dst_pass      = dst,
                .resource      = resource,
                .resource_kind = resource_kind,
                .flags         = RGCompiledHazardFlagMask(flag),
                .src_queue     = src_queue,
                .dst_queue     = dst_queue
            }
        );
    };

    auto push_texture_state = [](Moer::Array<TextureCompileState>& states, const RGTextureAccess& access, uint32_t pass_index) {
        uint32_t write_index = 0;
        for (uint32_t state_index = 0; state_index < states.size(); ++state_index) {
            const TextureCompileState state = states[state_index];
            if (access.range.Contains(state.range)) {
                continue;
            }
            states[write_index++] = state;
        }
        states.resize(write_index);
        states.push_back(TextureCompileState{access.range, access.state, access.queue, pass_index});
    };

    auto push_buffer_state = [](Moer::Array<BufferCompileState>& states, const RGBufferAccess& access, uint32_t pass_index) {
        uint32_t write_index = 0;
        for (uint32_t state_index = 0; state_index < states.size(); ++state_index) {
            const BufferCompileState state = states[state_index];
            if (access.range.Contains(state.range)) {
                continue;
            }
            states[write_index++] = state;
        }
        states.resize(write_index);
        states.push_back(BufferCompileState{access.range, access.state, access.queue, pass_index});
    };

    auto process_texture_access = [&](uint32_t pass_index, const RGTextureAccess& access) {
        RGResource& resource = CheckedResource(access.handle);
        resource.compile.RecordAccess(pass_index, RGTextureStateWrites(access.state));

        Moer::Array<TextureCompileState>& states = texture_states[access.handle.index];
        for (const TextureCompileState& state : states) {
            if (!state.range.Overlaps(access.range)) {
                continue;
            }
            if (RGTextureStateConflicts(state.state, access.state)) {
                add_dependency(
                    state.pass,
                    pass_index,
                    access.handle,
                    ERGResourceKind::Texture,
                    ERGCompiledHazardFlag::AccessConflict,
                    state.queue,
                    access.queue
                );
            }
            if (resource.transient.enabled && state.queue != access.queue) {
                add_dependency(
                    state.pass,
                    pass_index,
                    access.handle,
                    ERGResourceKind::Texture,
                    ERGCompiledHazardFlag::OwnerTransfer,
                    state.queue,
                    access.queue
                );
            }
        }
        if (resource.transient.enabled) {
            resource.transient.SetOwner(access.queue, pass_index);
        }
        push_texture_state(states, access, pass_index);
    };

    auto process_buffer_access = [&](uint32_t pass_index, const RGBufferAccess& access) {
        RGResource& resource = CheckedResource(access.handle);
        resource.compile.RecordAccess(pass_index, RGBufferStateWrites(access.state));

        Moer::Array<BufferCompileState>& states = buffer_states[access.handle.index];
        for (const BufferCompileState& state : states) {
            if (!state.range.Overlaps(access.range)) {
                continue;
            }
            if (RGBufferStateConflicts(state.state, access.state)) {
                add_dependency(
                    state.pass,
                    pass_index,
                    access.handle,
                    ERGResourceKind::Buffer,
                    ERGCompiledHazardFlag::AccessConflict,
                    state.queue,
                    access.queue
                );
            }
            if (resource.transient.enabled && state.queue != access.queue) {
                add_dependency(
                    state.pass,
                    pass_index,
                    access.handle,
                    ERGResourceKind::Buffer,
                    ERGCompiledHazardFlag::OwnerTransfer,
                    state.queue,
                    access.queue
                );
            }
        }
        if (resource.transient.enabled) {
            resource.transient.SetOwner(access.queue, pass_index);
        }
        push_buffer_state(states, access, pass_index);
    };

    for (uint32_t pass_index = 0; pass_index < m_passes.size(); ++pass_index) {
        RGPass& pass = m_passes[pass_index];
        if (pass.execution_mode == ERGPassExecutionMode::Serial) {
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
