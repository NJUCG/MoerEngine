#include "log/LogSystem.h"
#include "rendergraph/RenderGraph.h"

namespace Moer::Render::Tests {
namespace {

struct RGExecutionParams {
    DEFINE_RG_TEXTURE_ACCESS(texture, ETextureState::RENDER_TARGET);
    DEFINE_RG_BUFFER_ACCESS(buffer, EBufferState::UNORDERED_ACCESS);
    uint32_t prepared_value{0};
    uint32_t execution_count{0};
    bool     resources_allocated{false};

    DEFINE_RG_PARAMETER_ACCESS(texture, buffer);
};

struct RGNestedReadParams {
    DEFINE_RG_TEXTURE_ACCESS(texture, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_BUFFER_ACCESS(buffer, EBufferState::SHADER_RESOURCE);

    DEFINE_RG_PARAMETER_ACCESS(texture, buffer);
};

struct RGAccessArrayParams {
    DEFINE_RG_NESTED_PARAMETER(RGNestedReadParams, nested);
    DEFINE_RG_TEXTURE_ACCESS_ARRAY(textures);
    DEFINE_RG_BUFFER_ACCESS_ARRAY(buffers);

    DEFINE_RG_PARAMETER_ACCESS(nested, textures, buffers);
};

struct RGSerialParams {
    uint32_t execution_count{0};
};

bool HasHazard(
    const RGCompiledPlan& plan,
    uint32_t              src_pass,
    uint32_t              dst_pass,
    RenderGraphHandle     resource,
    ERGResourceKind       resource_kind
) {
    for (const RGCompiledHazardEdge& edge : plan.hazard_edges) {
        if (edge.src_pass == src_pass && edge.dst_pass == dst_pass && edge.resource == resource &&
            edge.resource_kind == resource_kind) {
            return true;
        }
    }
    return false;
}

const RGCompiledHazardEdge* FindHazard(
    const RGCompiledPlan& plan,
    uint32_t              src_pass,
    uint32_t              dst_pass,
    RenderGraphHandle     resource,
    ERGResourceKind       resource_kind
) {
    for (const RGCompiledHazardEdge& edge : plan.hazard_edges) {
        if (edge.src_pass == src_pass && edge.dst_pass == dst_pass && edge.resource == resource &&
            edge.resource_kind == resource_kind) {
            return &edge;
        }
    }
    return nullptr;
}

bool ValidatePassDomainHelpers() {
    if (!RGPassHasValidQueueFlags(ERGPassFlags::None) || !RGPassHasQueue(ERGPassFlags::Graphics) ||
        !RGPassHasQueue(ERGPassFlags::Compute) || !RGPassHasQueue(ERGPassFlags::Copy)) {
        LOG_ERROR(MOER_TEXT("RenderGraph pass queue flags were rejected"));
        return false;
    }
    if (RGPassHasQueue(ERGPassFlags::None) ||
        RGPassHasValidQueueFlags(ERGPassFlags::Graphics | ERGPassFlags::Compute)) {
        LOG_ERROR(MOER_TEXT("Invalid RenderGraph pass queue flags were accepted"));
        return false;
    }
    if (RGPassQueue(ERGPassFlags::None) != EQueueType::Ignore ||
        RGPassQueue(ERGPassFlags::Graphics) != EQueueType::Graphics ||
        RGPassQueue(ERGPassFlags::Compute) != EQueueType::Compute ||
        RGPassQueue(ERGPassFlags::Copy) != EQueueType::Copy) {
        LOG_ERROR(MOER_TEXT("RenderGraph pass queue flags mapped to the wrong queue"));
        return false;
    }
    return true;
}

bool ValidateRangeOverlapRules() {
    if (!RGBufferRange{.offset = 0, .size = 0}.Overlaps(RGBufferRange{.offset = 64, .size = 16})) {
        LOG_ERROR(MOER_TEXT("Whole-buffer RenderGraph range did not overlap a partial range"));
        return false;
    }
    if (RGBufferRange{.offset = 0, .size = 16}.Overlaps(RGBufferRange{.offset = 16, .size = 16})) {
        LOG_ERROR(MOER_TEXT("Adjacent RenderGraph buffer ranges overlapped"));
        return false;
    }
    if (!RGBufferRange{.offset = 8, .size = 16}.Overlaps(RGBufferRange{.offset = 16, .size = 8})) {
        LOG_ERROR(MOER_TEXT("Intersecting RenderGraph buffer ranges did not overlap"));
        return false;
    }

    const RGTextureRange mip0{.aspect = ETextureAspectFlags::COLOR, .mip_min = 0, .mip_count = 1};
    const RGTextureRange mip1{.aspect = ETextureAspectFlags::COLOR, .mip_min = 1, .mip_count = 1};
    const RGTextureRange mip0_depth{.aspect = ETextureAspectFlags::DEPTH_SLICE, .mip_min = 0, .mip_count = 1};
    if (mip0.Overlaps(mip1)) {
        LOG_ERROR(MOER_TEXT("Different RenderGraph texture mip ranges overlapped"));
        return false;
    }
    if (mip0.Overlaps(mip0_depth)) {
        LOG_ERROR(MOER_TEXT("Different RenderGraph texture aspects overlapped"));
        return false;
    }
    return true;
}

RGTextureDesc MakeColorTextureDesc(Extent3D extent, ETextureUsageFlags usage, uint8_t num_mips = 1) {
    RGTextureDesc desc{
        ETextureDimension::TEX_2D,
        usage,
        PF_R8G8B8A8_UNORM,
        EClearAttachment::COLOR,
        extent,
        num_mips,
        1,
        1
    };
    desc.aspect_flags = ETextureAspectFlags::COLOR;
    return desc;
}

} // namespace

int RunRenderGraphContractFoundationTest() {
    if (!ValidatePassDomainHelpers() || !ValidateRangeOverlapRules()) {
        return 1;
    }

    PooledTexturePool texture_pool{3};
    PooledBufferPool  buffer_pool{3};
    RenderGraph       graph(texture_pool, buffer_pool);

    const RenderGraphHandle texture = graph.CreateTexture(
        MOER_TEXT("rg_contract_texture"),
        MakeColorTextureDesc(
            Extent3D(16, 16, 1),
            ETextureUsageFlags::COLOR_ATTACHMENT | ETextureUsageFlags::SAMPLED,
            2
        )
    );
    RGBufferDesc contract_buffer_desc{};
    contract_buffer_desc.size   = 256;
    contract_buffer_desc.stride = 1;
    contract_buffer_desc.usage  = EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::TRANSFER_SRC;
    const RenderGraphHandle buffer = graph.CreateBuffer(MOER_TEXT("rg_contract_buffer"), contract_buffer_desc);

    auto* write_params    = graph.Alloc<RGExecutionParams>();
    write_params->texture = RGTextureView{
        .handle = texture,
        .range  = RGTextureRange{.aspect = ETextureAspectFlags::COLOR, .mip_min = 0, .mip_count = 1}
    };
    write_params->buffer = RGBufferView{.handle = buffer, .range = RGBufferRange{.offset = 32, .size = 32}};

    auto* access_array_params = graph.Alloc<RGAccessArrayParams>();
    auto* serial_params       = graph.Alloc<RGSerialParams>();

    access_array_params->textures       = graph.Alloc<RGTextureAccessArray>(2);
    access_array_params->buffers        = graph.Alloc<RGBufferAccessArray>(2);
    access_array_params->nested.texture = RGTextureView{
        .handle = texture,
        .range  = RGTextureRange{.aspect = ETextureAspectFlags::COLOR, .mip_min = 0, .mip_count = 1}
    };
    access_array_params->nested.buffer =
        RGBufferView{.handle = buffer, .range = RGBufferRange{.offset = 48, .size = 8}};

    access_array_params->textures->AddAccess(
        RGTextureView{
            .handle = texture,
            .range  = RGTextureRange{.aspect = ETextureAspectFlags::COLOR, .mip_min = 0, .mip_count = 1}
        },
        ETextureState::SHADER_RESOURCE
    );
    access_array_params->textures->AddAccess(
        RGTextureView{
            .handle = texture,
            .range  = RGTextureRange{.aspect = ETextureAspectFlags::COLOR, .mip_min = 1, .mip_count = 1}
        },
        ETextureState::SHADER_RESOURCE
    );
    access_array_params->buffers->AddAccess(
        RGBufferView{.handle = buffer, .range = RGBufferRange{.offset = 48, .size = 8}},
        EBufferState::SHADER_RESOURCE
    );
    access_array_params->buffers->AddAccess(
        RGBufferView{.handle = buffer, .range = RGBufferRange{.offset = 96, .size = 16}},
        EBufferState::SHADER_RESOURCE
    );

    graph.AddSetupPass(
        MOER_TEXT("PrepareContractFoundation"),
        [write_params](RGSetupContext& setup) {
            (void)setup;
            write_params->prepared_value = 42;
        }
    );
    graph.AddPass(
        MOER_TEXT("WriteContractResources"),
        write_params,
        ERGPassFlags::Graphics,
        [write_params, texture, buffer](RHICommandList&, RGContext context) {
            write_params->resources_allocated = context.Graph().GetTexture(texture).IsAllocated() &&
                                                context.Graph().GetBuffer(buffer).IsAllocated();
            if (write_params->prepared_value == 42) {
                ++write_params->execution_count;
            }
        }
    );

    graph.AddPass(
        MOER_TEXT("SerialContractFence"),
        serial_params,
        ERGPassFlags::None,
        [serial_params](RGContext context) {
            (void)context.Graph();
            ++serial_params->execution_count;
        }
    );

    graph.AddPass(
        MOER_TEXT("ReadAccessArrays"),
        access_array_params,
        ERGPassFlags::Compute,
        [](RHICommandList&, RGContext) {}
    );

    if (graph.GetPasses()[0].texture_accesses.size() != 1 ||
        graph.GetPasses()[0].buffer_accesses.size() != 1 ||
        graph.GetPasses()[2].texture_accesses.size() != 3 ||
        graph.GetPasses()[2].buffer_accesses.size() != 3) {
        LOG_ERROR(MOER_TEXT("RenderGraph pass accesses were not finalized when AddPass returned"));
        return 1;
    }

    graph.ExportTexture(texture, ETextureState::SHADER_RESOURCE, EQueueType::Graphics);
    graph.ExportBuffer(buffer, EBufferState::SHADER_RESOURCE, EQueueType::Compute);

    graph.Dispatch();

    const RGCompiledPlan& plan = graph.GetCompiledPlan();
    if (plan.hazard_edges.size() != 2) {
        LOG_ERROR(MOER_TEXT("RenderGraph foundation expected 2 hazards, got {}"), plan.hazard_edges.size());
        return 1;
    }
    if (plan.execution_batches.size() != 2 || plan.execution_batches[0].queue != EQueueType::Graphics ||
        plan.execution_batches[1].queue != EQueueType::Compute) {
        LOG_ERROR(MOER_TEXT("RenderGraph foundation compiled wrong execution batches"));
        return 1;
    }
    if (!HasHazard(plan, 0, 2, texture, ERGResourceKind::Texture) ||
        !HasHazard(plan, 0, 2, buffer, ERGResourceKind::Buffer)) {
        LOG_ERROR(MOER_TEXT("RenderGraph foundation compiled wrong hazard edges"));
        return 1;
    }
    const RGCompiledHazardEdge* texture_edge = FindHazard(plan, 0, 2, texture, ERGResourceKind::Texture);
    if (!texture_edge || !RGCompiledHazardHasFlag(*texture_edge, ERGCompiledHazardFlag::AccessConflict) ||
        !RGCompiledHazardHasFlag(*texture_edge, ERGCompiledHazardFlag::OwnerTransfer)) {
        LOG_ERROR(MOER_TEXT("RenderGraph foundation did not mark the texture owner-transfer hazard"));
        return 1;
    }
    const RGCompiledHazardEdge* buffer_edge = FindHazard(plan, 0, 2, buffer, ERGResourceKind::Buffer);
    if (!buffer_edge || !RGCompiledHazardHasFlag(*buffer_edge, ERGCompiledHazardFlag::AccessConflict) ||
        !RGCompiledHazardHasFlag(*buffer_edge, ERGCompiledHazardFlag::OwnerTransfer)) {
        LOG_ERROR(MOER_TEXT("RenderGraph foundation did not mark the buffer owner-transfer hazard"));
        return 1;
    }
    const RGResource& texture_resource = graph.GetResources()[texture.index];
    const RGResource& buffer_resource  = graph.GetResources()[buffer.index];
    if (texture_resource.compile.first_pass != 0 || texture_resource.compile.last_pass != 2 ||
        texture_resource.compile.access_write || buffer_resource.compile.first_pass != 0 ||
        buffer_resource.compile.last_pass != 2 || buffer_resource.compile.access_write) {
        LOG_ERROR(MOER_TEXT("RenderGraph foundation compiled wrong resource lifetime metadata"));
        return 1;
    }
    const RGPass& write_pass = graph.GetPasses()[0];
    const RGPass& read_pass  = graph.GetPasses()[2];
    if (read_pass.compile.last_pass != 0 ||
        read_pass.compile.last_pass_by_queue[static_cast<size_t>(EQueueType::Graphics)] != 0 ||
        write_pass.compile.next_pass_by_queue[static_cast<size_t>(EQueueType::Compute)] != 2) {
        LOG_ERROR(MOER_TEXT("RenderGraph foundation compiled wrong pass dependency metadata"));
        return 1;
    }
    if (!write_params->resources_allocated) {
        LOG_ERROR(MOER_TEXT("RenderGraph transient resources were not allocated before execution"));
        return 1;
    }
    if (graph.GetResources()[texture.index].texture || graph.GetResources()[buffer.index].buffer) {
        LOG_ERROR(MOER_TEXT("RenderGraph transient resources were not released back to the pool"));
        return 1;
    }
    if (texture_pool.LiveCount() == 0 || buffer_pool.LiveCount() == 0) {
        LOG_ERROR(MOER_TEXT("RenderGraph resource pools did not retain released resources"));
        return 1;
    }
    if (write_params->prepared_value != 42 || write_params->execution_count != 1 ||
        serial_params->execution_count != 1) {
        LOG_ERROR(MOER_TEXT("RenderGraph setup or execution lambda order is invalid"));
        return 1;
    }

    PooledTextureRef registered_texture = texture_pool.Allocate(
        MOER_TEXT("rg_registered_texture"),
        MakeColorTextureDesc(Extent3D(8, 8, 1), ETextureUsageFlags::SAMPLED)
    );
    RGBufferDesc registered_buffer_desc{};
    registered_buffer_desc.size   = 64;
    registered_buffer_desc.stride = 1;
    registered_buffer_desc.usage  = EBufferUsageFlags::UNORDERED_ACCESS;
    PooledBufferRef registered_buffer = buffer_pool.Allocate(MOER_TEXT("rg_registered_buffer"), registered_buffer_desc);

    RenderGraph             register_graph(texture_pool, buffer_pool);
    const RenderGraphHandle registered_texture_handle = register_graph.RegisterTexture(
        MOER_TEXT("rg_registered_texture"), registered_texture, EQueueType::Graphics
    );
    const RenderGraphHandle registered_buffer_handle = register_graph.RegisterBuffer(
        MOER_TEXT("rg_registered_buffer"), registered_buffer, EQueueType::Compute
    );
    if (register_graph.GetTexture(registered_texture_handle).Pooled() != registered_texture ||
        register_graph.GetBuffer(registered_buffer_handle).Pooled() != registered_buffer) {
        LOG_ERROR(MOER_TEXT("RenderGraph did not register pooled resources directly"));
        return 1;
    }

    registered_texture.reset();
    registered_buffer.reset();
    register_graph.Reset();
    for (uint32_t frame = 0; frame < 4; ++frame) {
        texture_pool.Tick();
        buffer_pool.Tick();
    }
    if (texture_pool.LiveCount() != 0 || buffer_pool.LiveCount() != 0) {
        LOG_ERROR(MOER_TEXT("RenderGraph resource pools did not destroy multi-frame idle resources"));
        return 1;
    }

    LOG_INFO(MOER_TEXT("RenderGraph contract foundation test passed, hazards={}"), plan.hazard_edges.size());
    return 0;
}

} // namespace Moer::Render::Tests