#include "rendergraph/RenderGraph.h"

#include <algorithm>

namespace Moer {

uint64_t RGFrameContext::NextGraphSequence() {
    return m_next_graph_sequence++;
}

void RGFrameContext::PublishReceipt(const RGFrameReceipt& receipt) {
    for (auto& current : m_receipts) {
        if (current.resource == receipt.resource) {
            current = receipt;
            return;
        }
    }
    m_receipts.push_back(receipt);
}

const RGFrameReceipt* RGFrameContext::FindReceipt(RenderGraphHandle resource) const {
    for (const auto& receipt : m_receipts) {
        if (receipt.resource == resource) {
            return &receipt;
        }
    }
    return nullptr;
}

void RGFrameContext::Reset(uint64_t frame_sequence) {
    m_frame_sequence = frame_sequence;
    m_next_graph_sequence = 0;
    m_receipts.clear();
}

void RGSetupContext::ReadTexture(
    RenderGraphHandle handle,
    Render::ETextureState state,
    RGTextureRange range,
    Render::EQueueType queue,
    bool bindless
) {
    m_graph.AddTextureAccess(m_pass_index, RGTextureAccess{handle, range, ERGAccessMode::Read, state, queue, bindless});
}

void RGSetupContext::WriteTexture(
    RenderGraphHandle handle,
    Render::ETextureState state,
    RGTextureRange range,
    Render::EQueueType queue,
    bool bindless
) {
    m_graph.AddTextureAccess(m_pass_index, RGTextureAccess{handle, range, ERGAccessMode::Write, state, queue, bindless});
}

void RGSetupContext::ReadWriteTexture(
    RenderGraphHandle handle,
    Render::ETextureState state,
    RGTextureRange range,
    Render::EQueueType queue,
    bool bindless
) {
    m_graph.AddTextureAccess(m_pass_index, RGTextureAccess{handle, range, ERGAccessMode::ReadWrite, state, queue, bindless});
}

void RGSetupContext::ReadBuffer(
    RenderGraphHandle handle,
    Render::EBufferState state,
    RGBufferRange range,
    Render::EQueueType queue,
    bool bindless
) {
    m_graph.AddBufferAccess(m_pass_index, RGBufferAccess{handle, range, ERGAccessMode::Read, state, queue, bindless});
}

void RGSetupContext::WriteBuffer(
    RenderGraphHandle handle,
    Render::EBufferState state,
    RGBufferRange range,
    Render::EQueueType queue,
    bool bindless
) {
    m_graph.AddBufferAccess(m_pass_index, RGBufferAccess{handle, range, ERGAccessMode::Write, state, queue, bindless});
}

void RGSetupContext::ReadWriteBuffer(
    RenderGraphHandle handle,
    Render::EBufferState state,
    RGBufferRange range,
    Render::EQueueType queue,
    bool bindless
) {
    m_graph.AddBufferAccess(m_pass_index, RGBufferAccess{handle, range, ERGAccessMode::ReadWrite, state, queue, bindless});
}

bool RGPassHasSingleExecutionDomain(ERGPassFlags flags) {
    uint32_t count = 0;
    count += EnumHasAnyFlag(flags, ERGPassFlags::Graphics) ? 1 : 0;
    count += EnumHasAnyFlag(flags, ERGPassFlags::Compute) ? 1 : 0;
    count += EnumHasAnyFlag(flags, ERGPassFlags::Copy) ? 1 : 0;
    count += EnumHasAnyFlag(flags, ERGPassFlags::Raytracing) ? 1 : 0;
    return count == 1;
}

Render::EQueueType RGPassQueue(ERGPassFlags flags) {
    assert(RGPassHasSingleExecutionDomain(flags));
    if (EnumHasAnyFlag(flags, ERGPassFlags::Compute)) {
        return Render::EQueueType::Compute;
    }
    if (EnumHasAnyFlag(flags, ERGPassFlags::Copy)) {
        return Render::EQueueType::Copy;
    }
    return Render::EQueueType::Graphics;
}

EPassType RGPassType(ERGPassFlags flags) {
    assert(RGPassHasSingleExecutionDomain(flags));
    if (EnumHasAnyFlag(flags, ERGPassFlags::Compute)) {
        return EPassType::Compute;
    }
    if (EnumHasAnyFlag(flags, ERGPassFlags::Copy)) {
        return EPassType::Copy;
    }
    if (EnumHasAnyFlag(flags, ERGPassFlags::Raytracing)) {
        return EPassType::Raytracing;
    }
    return EPassType::Graphics;
}

RenderGraph::RenderGraph(RGFrameContext& frame_context) :
    m_frame_context(frame_context),
    m_graph_sequence(frame_context.NextGraphSequence()) {}

RenderGraph::~RenderGraph() {
    Reset();
}

RenderGraphHandle RenderGraph::CreateTexture(std::string_view name, const RGTextureDesc& desc) {
    assert(m_phase == Phase::Setup);
    RGResource resource{};
    resource.name = std::string(name);
    resource.kind = ERGResourceKind::Texture;
    resource.texture_desc = desc;
    return AddResource(std::move(resource));
}

RenderGraphHandle RenderGraph::CreateBuffer(std::string_view name, const RGBufferDesc& desc) {
    assert(m_phase == Phase::Setup);
    RGResource resource{};
    resource.name = std::string(name);
    resource.kind = ERGResourceKind::Buffer;
    resource.buffer_desc = desc;
    return AddResource(std::move(resource));
}

RenderGraphHandle RenderGraph::ImportTexture(
    std::string_view name,
    RHITextureRef texture,
    Render::ETextureState initial_state,
    Render::EQueueType owner_queue
) {
    assert(m_phase == Phase::Setup);
    assert(texture && initial_state != Render::ETextureState::UNDEFINED);
    RGResource resource{};
    resource.name = std::string(name);
    resource.kind = ERGResourceKind::Texture;
    resource.imported = true;
    resource.imported_texture = texture;
    resource.initial_texture_state = initial_state;
    resource.owner_queue = owner_queue;
    return AddResource(std::move(resource));
}

RenderGraphHandle RenderGraph::ImportBuffer(
    std::string_view name,
    RHIBufferRef buffer,
    Render::EBufferState initial_state,
    Render::EQueueType owner_queue
) {
    assert(m_phase == Phase::Setup);
    assert(buffer && initial_state != Render::EBufferState::UNDEFINED);
    RGResource resource{};
    resource.name = std::string(name);
    resource.kind = ERGResourceKind::Buffer;
    resource.imported = true;
    resource.imported_buffer = buffer;
    resource.initial_buffer_state = initial_state;
    resource.owner_queue = owner_queue;
    return AddResource(std::move(resource));
}

void RenderGraph::ExportTexture(RenderGraphHandle handle, Render::ETextureState final_state, Render::EQueueType owner_queue) {
    auto& resource = CheckedResource(handle);
    assert(resource.kind == ERGResourceKind::Texture);
    resource.exported = true;
    resource.final_texture_state = final_state;
    resource.owner_queue = owner_queue;
}

void RenderGraph::ExportBuffer(RenderGraphHandle handle, Render::EBufferState final_state, Render::EQueueType owner_queue) {
    auto& resource = CheckedResource(handle);
    assert(resource.kind == ERGResourceKind::Buffer);
    resource.exported = true;
    resource.final_buffer_state = final_state;
    resource.owner_queue = owner_queue;
}

void RenderGraph::AddSetupPass(std::string_view name, SetupExecute&& setup) {
    assert(m_phase == Phase::Setup);
    const uint32_t pass_index = static_cast<uint32_t>(m_passes.size());
    m_setup_passes.push_back(RGSetupPass{std::string(name)});
    m_passes.push_back(RGPass{.name = std::string(name)});
    RGSetupContext context(*this, pass_index);
    setup(context);
}

void RenderGraph::AddPassInternal(
    void* parameters,
    std::type_index type,
    uint32_t size,
    ERGPassFlags flags,
    RGPass::Execute&& execute
) {
    assert(m_phase == Phase::Setup);
    assert(RGPassHasSingleExecutionDomain(flags));
    if (m_passes.empty() || m_passes.back().execute) {
        m_setup_passes.push_back(RGSetupPass{"UnnamedPass"});
        m_passes.push_back(RGPass{.name = "UnnamedPass"});
    }

    auto& pass = m_passes.back();
    pass.parameters = parameters;
    pass.parameter_type = type;
    pass.parameter_size = size;
    pass.flags = flags;
    pass.execute = std::move(execute);
}

void RenderGraph::Compile() {
    if (m_phase != Phase::Setup) {
        return;
    }
    ValidateSetup();
    BuildHazards();
    m_phase = Phase::Compiled;
}

void RenderGraph::Dispatch(RHICommandList* cmd_list) {
    Compile();
    if (cmd_list) {
        RGContext context(*this);
        for (auto& pass : m_passes) {
            pass.execute(*cmd_list, context);
        }
    }

    for (const auto& resource : m_resources) {
        if (!resource.exported) {
            continue;
        }
        const auto handle = RenderGraphHandle(static_cast<RenderGraphHandle::Index>(&resource - m_resources.data()));
        m_frame_context.PublishReceipt(RGFrameReceipt{
            .resource = handle,
            .owner_queue = resource.owner_queue,
            .texture_state = resource.final_texture_state,
            .buffer_state = resource.final_buffer_state,
            .completion_value = m_graph_sequence
        });
    }

    m_phase = Phase::Dispatched;
}

void RenderGraph::Reset() {
    for (auto& allocation : m_allocations) {
        if (allocation.ptr && allocation.destroy) {
            allocation.destroy(allocation.ptr);
        }
    }
    m_allocations.clear();
    m_resources.clear();
    m_setup_passes.clear();
    m_passes.clear();
    m_compiled_plan.hazard_edges.clear();
    m_phase = Phase::Setup;
}

void RenderGraph::AddTextureAccess(uint32_t pass_index, const RGTextureAccess& access) {
    assert(pass_index < m_passes.size());
    const auto& resource = CheckedResource(access.handle);
    assert(resource.kind == ERGResourceKind::Texture);
    assert(access.state != Render::ETextureState::UNDEFINED);
    m_passes[pass_index].texture_accesses.push_back(access);
}

void RenderGraph::AddBufferAccess(uint32_t pass_index, const RGBufferAccess& access) {
    assert(pass_index < m_passes.size());
    const auto& resource = CheckedResource(access.handle);
    assert(resource.kind == ERGResourceKind::Buffer);
    assert(access.state != Render::EBufferState::UNDEFINED);
    m_passes[pass_index].buffer_accesses.push_back(access);
}

RGResource& RenderGraph::CheckedResource(RenderGraphHandle handle) {
    assert(handle.IsInitialized() && handle.index < m_resources.size());
    return m_resources[handle.index];
}

const RGResource& RenderGraph::CheckedResource(RenderGraphHandle handle) const {
    assert(handle.IsInitialized() && handle.index < m_resources.size());
    return m_resources[handle.index];
}

RenderGraphHandle RenderGraph::AddResource(RGResource&& resource) {
    assert(m_resources.size() < RenderGraphHandle::UNINITIALIZED);
    for (const auto& existing : m_resources) {
        assert(existing.name != resource.name && "RenderGraph resource names must be unique");
    }
    const auto handle = RenderGraphHandle(static_cast<RenderGraphHandle::Index>(m_resources.size()));
    m_resources.push_back(std::move(resource));
    return handle;
}

void RenderGraph::ValidateSetup() const {
    for (const auto& pass : m_passes) {
        assert(pass.execute && "Every setup pass must have one AddPass execution lambda");
        assert(RGPassHasSingleExecutionDomain(pass.flags));
        for (const auto& access : pass.texture_accesses) {
            const auto& resource = CheckedResource(access.handle);
            assert(resource.kind == ERGResourceKind::Texture);
            if (!RGAccessWrites(access.mode)) {
                assert((resource.imported || resource.initial_texture_state != Render::ETextureState::UNDEFINED || !resource.imported) && "First read of an unknown external texture is invalid");
            }
        }
        for (const auto& access : pass.buffer_accesses) {
            const auto& resource = CheckedResource(access.handle);
            assert(resource.kind == ERGResourceKind::Buffer);
            if (!RGAccessWrites(access.mode)) {
                assert((resource.imported || resource.initial_buffer_state != Render::EBufferState::UNDEFINED || !resource.imported) && "First read of an unknown external buffer is invalid");
            }
        }
    }
}

void RenderGraph::BuildHazards() {
    m_compiled_plan.hazard_edges.clear();
    for (uint32_t dst = 0; dst < m_passes.size(); ++dst) {
        const auto& dst_pass = m_passes[dst];
        for (uint32_t src = 0; src < dst; ++src) {
            const auto& src_pass = m_passes[src];
            for (const auto& src_access : src_pass.texture_accesses) {
                for (const auto& dst_access : dst_pass.texture_accesses) {
                    if (src_access.handle == dst_access.handle && src_access.range.Overlaps(dst_access.range) && RGAccessConflicts(src_access.mode, dst_access.mode)) {
                        m_compiled_plan.hazard_edges.push_back(RGCompiledHazardEdge{src, dst, src_access.handle, ERGResourceKind::Texture});
                    }
                }
            }
            for (const auto& src_access : src_pass.buffer_accesses) {
                for (const auto& dst_access : dst_pass.buffer_accesses) {
                    if (src_access.handle == dst_access.handle && src_access.range.Overlaps(dst_access.range) && RGAccessConflicts(src_access.mode, dst_access.mode)) {
                        m_compiled_plan.hazard_edges.push_back(RGCompiledHazardEdge{src, dst, src_access.handle, ERGResourceKind::Buffer});
                    }
                }
            }
        }
    }
}

} // namespace Moer
