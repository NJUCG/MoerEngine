#include "rendergraph/RenderGraph.h"

#include "misc/Assert.h"
#include "misc/Hash.h"

#include <unordered_set>
#include <utility>

namespace Moer {

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

RenderGraph::~RenderGraph() {
    Reset();
}

RenderGraphHandle RenderGraph::CreateTexture(StringView name, const RGTextureDesc& desc) {
    assert(m_phase == Phase::Setup);
    RGResource resource{};
    resource.name = String(name);
    resource.kind = ERGResourceKind::Texture;
    resource.texture_desc = desc;
    return AddResource(std::move(resource));
}

RenderGraphHandle RenderGraph::CreateBuffer(StringView name, const RGBufferDesc& desc) {
    assert(m_phase == Phase::Setup);
    RGResource resource{};
    resource.name = String(name);
    resource.kind = ERGResourceKind::Buffer;
    resource.buffer_desc = desc;
    return AddResource(std::move(resource));
}

RenderGraphHandle RenderGraph::ImportTexture(
    StringView name,
    Render::TextureRef texture,
    Render::EQueueType owner_queue
) {
    assert(m_phase == Phase::Setup);
    assert(texture);
    RGResource resource{};
    resource.name = String(name);
    resource.kind = ERGResourceKind::Texture;
    resource.imported = true;
    resource.imported_texture = texture;
    resource.owner_queue = owner_queue;
    return AddResource(std::move(resource));
}

RenderGraphHandle RenderGraph::ImportBuffer(
    StringView name,
    Render::BufferRef buffer,
    Render::EQueueType owner_queue
) {
    assert(m_phase == Phase::Setup);
    assert(buffer);
    RGResource resource{};
    resource.name = String(name);
    resource.kind = ERGResourceKind::Buffer;
    resource.imported = true;
    resource.imported_buffer = buffer;
    resource.owner_queue = owner_queue;
    return AddResource(std::move(resource));
}

void RenderGraph::ExportTexture(RenderGraphHandle handle, Render::ETextureState final_state, Render::EQueueType owner_queue) {
    auto& resource = CheckedResource(handle);
    assert(resource.kind == ERGResourceKind::Texture);
    assert(final_state != Render::ETextureState::UNDEFINED);
    resource.exported = true;
    resource.final_texture_state = final_state;
    resource.owner_queue = owner_queue;
}

void RenderGraph::ExportBuffer(RenderGraphHandle handle, Render::EBufferState final_state, Render::EQueueType owner_queue) {
    auto& resource = CheckedResource(handle);
    assert(resource.kind == ERGResourceKind::Buffer);
    assert(final_state != Render::EBufferState::UNDEFINED);
    resource.exported = true;
    resource.final_buffer_state = final_state;
    resource.owner_queue = owner_queue;
}

void RenderGraph::AddSetupPass(StringView name, SetupExecute&& setup) {
    assert(m_phase == Phase::Setup);
    m_setup_passes.push_back(RGSetupPass{String(name), std::move(setup)});
}

uint32_t RenderGraph::AddPassInternal(
    String name,
    void* parameters,
    std::type_index type,
    uint32_t size,
    ERGPassFlags flags,
    ERGPassExecutionMode execution_mode,
    RGPass::CollectAccess&& collect_access,
    RGPass::ParallelExecute&& parallel_execute,
    RGPass::SerialExecute&& serial_execute
) {
    assert(m_phase == Phase::Setup);
    assert(RGPassHasValidQueueFlags(flags));
    assert(
        (execution_mode == ERGPassExecutionMode::Parallel && RGPassHasQueue(flags)) ||
        (execution_mode == ERGPassExecutionMode::Serial && flags == ERGPassFlags::None)
    );
    const uint32_t pass_index = static_cast<uint32_t>(m_passes.size());
    auto& pass = m_passes.emplace_back();
    pass.name = std::move(name);
    pass.parameters = parameters;
    pass.parameter_type = type;
    pass.parameter_size = size;
    pass.flags = flags;
    pass.execution_mode = execution_mode;
    pass.collect_access = std::move(collect_access);
    pass.parallel_execute = std::move(parallel_execute);
    pass.serial_execute = std::move(serial_execute);
    return pass_index;
}

void RenderGraph::Compile() {
    if (m_phase != Phase::Setup) {
        return;
    }
    RunSetupPasses();
    CollectPassAccesses();
    ValidateSetup();
    BuildHazards();
    m_phase = Phase::Compiled;
}

void RenderGraph::Dispatch(RHICommandList* cmd_list) {
    Compile();
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

    m_phase = Phase::Dispatched;
}

void RenderGraph::EmitFinalTrackedStates(RHICommandList& cmd_list) const {
    Moer::Array<Render::TrackedTextureState> textures{};
    Moer::Array<Render::TrackedBufferState> buffers{};

    auto texture_access_write = [this](RenderGraphHandle handle) {
        bool access_write = false;
        for (const RGPass& pass : m_passes) {
            for (const RGTextureAccess& access : pass.texture_accesses) {
                if (access.handle == handle) {
                    access_write = RGAccessWrites(access.mode);
                }
            }
        }
        return access_write;
    };
    auto buffer_access_write = [this](RenderGraphHandle handle) {
        bool access_write = false;
        for (const RGPass& pass : m_passes) {
            for (const RGBufferAccess& access : pass.buffer_accesses) {
                if (access.handle == handle) {
                    access_write = RGAccessWrites(access.mode);
                }
            }
        }
        return access_write;
    };

    for (uint32_t resource_index = 0; resource_index < m_resources.size(); ++resource_index) {
        const auto& resource = m_resources[resource_index];
        if (!resource.exported || !resource.imported) {
            continue;
        }

        const auto handle = RenderGraphHandle(static_cast<RenderGraphHandle::Index>(resource_index));
        if (resource.kind == ERGResourceKind::Texture) {
            auto* texture = resource.imported_texture.Get();
            if (texture == nullptr) {
                continue;
            }
            textures.emplace_back(Render::TrackedTextureState{
                .texture = Render::TextureView(
                    texture,
                    texture->GetFormat(),
                    0,
                    static_cast<uint8>(texture->GetNumMips())
                ),
                .state = resource.final_texture_state,
                .owner_queue = resource.owner_queue,
                .access_write = texture_access_write(handle)
            });
            continue;
        }

        auto* buffer = resource.imported_buffer.Get();
        if (buffer == nullptr) {
            continue;
        }
        buffers.emplace_back(Render::TrackedBufferState{
            .buffer = buffer->GetView(),
            .state = resource.final_buffer_state,
            .owner_queue = resource.owner_queue,
            .access_write = buffer_access_write(handle)
        });
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
    m_resources.clear();
    m_setup_passes.clear();
    m_passes.clear();
    m_compiled_plan.hazard_edges.clear();
    m_setup_executed = false;
    m_phase = Phase::Setup;
}

void RenderGraph::AddTextureAccess(uint32_t pass_index, const RGTextureAccess& access) {
    assert(pass_index < m_passes.size());
    const auto& resource = CheckedResource(access.handle);
    assert(resource.kind == ERGResourceKind::Texture);
    assert(access.state != Render::ETextureState::UNDEFINED);
    assert(access.queue != Render::EQueueType::Ignore);
    m_passes[pass_index].texture_accesses.push_back(access);
}

void RenderGraph::AddBufferAccess(uint32_t pass_index, const RGBufferAccess& access) {
    assert(pass_index < m_passes.size());
    const auto& resource = CheckedResource(access.handle);
    assert(resource.kind == ERGResourceKind::Buffer);
    assert(access.state != Render::EBufferState::UNDEFINED);
    assert(access.queue != Render::EQueueType::Ignore);
    m_passes[pass_index].buffer_accesses.push_back(access);
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

void RenderGraph::CollectPassAccesses() {
    for (uint32_t pass_index = 0; pass_index < m_passes.size(); ++pass_index) {
        auto& pass = m_passes[pass_index];
        pass.texture_accesses.clear();
        pass.buffer_accesses.clear();
        if (!pass.collect_access) {
            continue;
        }
        RGParameterAccessCollector collector{};
        pass.collect_access(pass.parameters, collector);
        for (const RGTextureAccess& access : collector.Textures()) {
            AddTextureAccess(pass_index, access);
        }
        for (const RGBufferAccess& access : collector.Buffers()) {
            AddBufferAccess(pass_index, access);
        }
    }
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
            if (!RGAccessWrites(access.mode) && !resource.imported && !has_texture_write_before_pass[access.handle.index]) {
                MOER_ASSERT(
                    false,
                    "Graph-created texture handle {} read before any pass writes to it",
                    access.handle.index
                );
            }
            if (RGAccessWrites(access.mode)) {
                has_texture_write_before_pass[access.handle.index] = true;
            }
        }
        for (const auto& access : pass.buffer_accesses) {
            const auto& resource = CheckedResource(access.handle);
            assert(resource.kind == ERGResourceKind::Buffer);
            if (!RGAccessWrites(access.mode) && !resource.imported && !has_buffer_write_before_pass[access.handle.index]) {
                MOER_ASSERT(
                    false,
                    "Graph-created buffer handle {} read before any pass writes to it",
                    access.handle.index
                );
            }
            if (RGAccessWrites(access.mode)) {
                has_buffer_write_before_pass[access.handle.index] = true;
            }
        }
    }
}

void RenderGraph::BuildHazards() {
    m_compiled_plan.hazard_edges.clear();
    struct HazardKey {
        uint32_t src{0};
        uint32_t dst{0};
        uint32_t resource{0};
        ERGResourceKind resource_kind{ERGResourceKind::Texture};

        bool operator==(const HazardKey& other) const {
            return src == other.src && dst == other.dst && resource == other.resource &&
                   resource_kind == other.resource_kind;
        }
    };
    struct HazardKeyHash {
        size_t operator()(const HazardKey& key) const {
            uint64_t hash = key.src;
            HashCombine(hash, key.dst);
            HashCombine(hash, key.resource);
            HashCombine(hash, static_cast<uint8_t>(key.resource_kind));
            return hash;
        }
    };

    std::unordered_set<HazardKey, HazardKeyHash> hazard_keys;
    const auto add_hazard = [this, &hazard_keys](
                                uint32_t src,
                                uint32_t dst,
                                RenderGraphHandle resource,
                                ERGResourceKind resource_kind
                            ) {
        if (hazard_keys.insert(HazardKey{src, dst, resource.index, resource_kind}).second) {
            m_compiled_plan.hazard_edges.push_back(RGCompiledHazardEdge{src, dst, resource, resource_kind});
        }
    };

    for (uint32_t dst = 0; dst < m_passes.size(); ++dst) {
        const auto& dst_pass = m_passes[dst];
        for (uint32_t src = 0; src < dst; ++src) {
            const auto& src_pass = m_passes[src];
            for (const auto& src_access : src_pass.texture_accesses) {
                for (const auto& dst_access : dst_pass.texture_accesses) {
                    if (src_access.handle == dst_access.handle && src_access.range.Overlaps(dst_access.range) && RGAccessConflicts(src_access.mode, dst_access.mode)) {
                        add_hazard(src, dst, src_access.handle, ERGResourceKind::Texture);
                    }
                }
            }
            for (const auto& src_access : src_pass.buffer_accesses) {
                for (const auto& dst_access : dst_pass.buffer_accesses) {
                    if (src_access.handle == dst_access.handle && src_access.range.Overlaps(dst_access.range) && RGAccessConflicts(src_access.mode, dst_access.mode)) {
                        add_hazard(src, dst, src_access.handle, ERGResourceKind::Buffer);
                    }
                }
            }
        }
    }
}

} // namespace Moer
