#include "log/LogSystem.h"
#include "rendergraph/RenderGraph.h"

namespace Moer::Render::Tests {
namespace {

struct RGTextureOnlyParams {
    RGTextureView texture;

    void DeclareRGAccess(RGParameterAccessCollector& collector) const {
        collector.AddTexture(texture);
    }
};

struct RGBufferOnlyParams {
    RGBufferView buffer;

    void DeclareRGAccess(RGParameterAccessCollector& collector) const {
        collector.AddBuffer(buffer);
    }
};

struct RGExecutionParams {
    RGTextureView texture;
    RGBufferView  buffer;
    uint32_t      prepared_value{0};
    uint32_t      execution_count{0};

    void DeclareRGAccess(RGParameterAccessCollector& collector) const {
        collector.AddTexture(texture);
        collector.AddBuffer(buffer);
    }
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

bool ValidatePassDomainHelpers() {
    if (!RGPassHasSingleExecutionDomain(ERGPassFlags::Graphics) ||
        !RGPassHasSingleExecutionDomain(ERGPassFlags::Compute) ||
        !RGPassHasSingleExecutionDomain(ERGPassFlags::Copy) ||
        !RGPassHasSingleExecutionDomain(ERGPassFlags::Raytracing)) {
        LOG_ERROR(MOER_TEXT("Single RenderGraph pass domain was rejected"));
        return false;
    }
    if (RGPassHasSingleExecutionDomain(ERGPassFlags::None) ||
        RGPassHasSingleExecutionDomain(ERGPassFlags::Graphics | ERGPassFlags::Compute)) {
        LOG_ERROR(MOER_TEXT("Invalid RenderGraph pass domain was accepted"));
        return false;
    }
    if (RGPassQueue(ERGPassFlags::Graphics) != EQueueType::Graphics ||
        RGPassQueue(ERGPassFlags::Compute) != EQueueType::Compute ||
        RGPassQueue(ERGPassFlags::Copy) != EQueueType::Copy) {
        LOG_ERROR(MOER_TEXT("RenderGraph pass domain mapped to the wrong queue"));
        return false;
    }
    if (RGPassType(ERGPassFlags::Raytracing) != EPassType::Raytracing) {
        LOG_ERROR(MOER_TEXT("RenderGraph raytracing domain mapped to the wrong pass type"));
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

} // namespace

int RunRenderGraphContractFoundationTest() {
    if (!ValidatePassDomainHelpers() || !ValidateRangeOverlapRules()) {
        return 1;
    }

    RGFrameContext frame_context(7);
    RenderGraph    graph(frame_context);

    const RenderGraphHandle texture = graph.CreateTexture(
        "rg_contract_texture",
        RGTextureDesc{
            .extent = Extent3D(16, 16, 1),
            .format = PF_R8G8B8A8_UNORM,
            .usage = ETextureUsageFlags::COLOR_ATTACHMENT | ETextureUsageFlags::SAMPLED,
            .mip_levels = 2,
            .array_layers = 1
        }
    );
    const RenderGraphHandle buffer = graph.CreateBuffer(
        "rg_contract_buffer",
        RGBufferDesc{.size = 256, .usage = EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::TRANSFER_SRC}
    );

    auto* write_params = graph.Alloc<RGExecutionParams>();
    write_params->texture = RGTextureView{
        .handle = texture,
        .range = RGTextureRange{.aspect = ETextureAspectFlags::COLOR, .mip_min = 0, .mip_count = 1},
        .access = ERGAccessMode::Write,
        .state = ETextureState::RENDER_TARGET,
        .queue = EQueueType::Graphics
    };
    write_params->buffer = RGBufferView{
        .handle = buffer,
        .range = RGBufferRange{.offset = 32, .size = 32},
        .access = ERGAccessMode::Write,
        .state = EBufferState::UNORDERED_ACCESS,
        .queue = EQueueType::Compute
    };

    graph.AddSetupPass("PrepareContractFoundation", [write_params](RGSetupContext& setup) {
        (void)setup.Graph();
        write_params->prepared_value = 42;
    });
    graph.AddPass("WriteContractResources", write_params, ERGPassFlags::Graphics, [write_params](RHICommandList&, RGContext context) {
        (void)context.Graph();
        if (write_params->prepared_value == 42) {
            ++write_params->execution_count;
        }
    });

    auto* texture_read_params = graph.Alloc<RGTextureOnlyParams>();
    texture_read_params->texture = RGTextureView{
        .handle = texture,
        .range = RGTextureRange{.aspect = ETextureAspectFlags::COLOR, .mip_min = 0, .mip_count = 1},
        .access = ERGAccessMode::Read,
        .state = ETextureState::SHADER_RESOURCE,
        .queue = EQueueType::Graphics,
        .bindless = true
    };
    graph.AddPass("ReadWrittenTexture", texture_read_params, ERGPassFlags::Compute, [](RHICommandList&, RGContext) {});

    auto* texture_read_disjoint_params = graph.Alloc<RGTextureOnlyParams>();
    texture_read_disjoint_params->texture = RGTextureView{
        .handle = texture,
        .range = RGTextureRange{.aspect = ETextureAspectFlags::COLOR, .mip_min = 1, .mip_count = 1},
        .access = ERGAccessMode::Read,
        .state = ETextureState::SHADER_RESOURCE,
        .queue = EQueueType::Graphics
    };
    graph.AddPass("ReadDisjointTextureMip", texture_read_disjoint_params, ERGPassFlags::Compute, [](RHICommandList&, RGContext) {});

    auto* buffer_read_params = graph.Alloc<RGBufferOnlyParams>();
    buffer_read_params->buffer = RGBufferView{
        .handle = buffer,
        .range = RGBufferRange{.offset = 48, .size = 8},
        .access = ERGAccessMode::Read,
        .state = EBufferState::SHADER_RESOURCE,
        .queue = EQueueType::Compute,
        .bindless = true
    };
    graph.AddPass("ReadWrittenBufferRange", buffer_read_params, ERGPassFlags::Compute, [](RHICommandList&, RGContext) {});

    auto* buffer_read_disjoint_params = graph.Alloc<RGBufferOnlyParams>();
    buffer_read_disjoint_params->buffer = RGBufferView{
        .handle = buffer,
        .range = RGBufferRange{.offset = 96, .size = 16},
        .access = ERGAccessMode::Read,
        .state = EBufferState::SHADER_RESOURCE,
        .queue = EQueueType::Compute
    };
    graph.AddPass("ReadDisjointBufferRange", buffer_read_disjoint_params, ERGPassFlags::Compute, [](RHICommandList&, RGContext) {});

    graph.ExportTexture(texture, ETextureState::SHADER_RESOURCE, EQueueType::Graphics);
    graph.ExportBuffer(buffer, EBufferState::SHADER_RESOURCE, EQueueType::Compute);
    graph.Compile();

    const RGCompiledPlan& plan = graph.GetCompiledPlan();
    if (plan.hazard_edges.size() != 2) {
        LOG_ERROR(MOER_TEXT("RenderGraph foundation expected 2 hazards, got {}"), plan.hazard_edges.size());
        return 1;
    }
    if (!HasHazard(plan, 0, 1, texture, ERGResourceKind::Texture) ||
        !HasHazard(plan, 0, 3, buffer, ERGResourceKind::Buffer)) {
        LOG_ERROR(MOER_TEXT("RenderGraph foundation compiled wrong hazard edges"));
        return 1;
    }

    RHICommandList command_list(EQueueType::Graphics);
    graph.Dispatch(&command_list);
    if (write_params->prepared_value != 42 || write_params->execution_count != 1) {
        LOG_ERROR(MOER_TEXT("RenderGraph setup or execution lambda order is invalid"));
        return 1;
    }

    const RGFrameReceipt* texture_receipt = frame_context.FindReceipt(texture);
    const RGFrameReceipt* buffer_receipt = frame_context.FindReceipt(buffer);
    if (texture_receipt == nullptr || texture_receipt->texture_state != ETextureState::SHADER_RESOURCE ||
        texture_receipt->owner_queue != EQueueType::Graphics) {
        LOG_ERROR(MOER_TEXT("RenderGraph texture export receipt is invalid"));
        return 1;
    }
    if (buffer_receipt == nullptr || buffer_receipt->buffer_state != EBufferState::SHADER_RESOURCE ||
        buffer_receipt->owner_queue != EQueueType::Compute) {
        LOG_ERROR(MOER_TEXT("RenderGraph buffer export receipt is invalid"));
        return 1;
    }

    LOG_INFO(MOER_TEXT("RenderGraph contract foundation test passed, hazards={}"), plan.hazard_edges.size());
    return 0;
}

} // namespace Moer::Render::Tests