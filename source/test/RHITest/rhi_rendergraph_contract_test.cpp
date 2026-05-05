#include "log/LogSystem.h"
#include "rendergraph/RenderGraph.h"

namespace Moer::Render::Tests {
namespace {

struct RGExecutionParams {
    DEFINE_RG_TEXTURE_ACCESS(texture);
    DEFINE_RG_BUFFER_ACCESS(buffer);
    uint32_t      prepared_value{0};
    uint32_t      execution_count{0};

    DEFINE_RG_PARAMETER_ACCESS(texture, buffer);
};

struct RGAccessArrayParams {
    DEFINE_RG_TEXTURE_ACCESS_ARRAY(textures);
    DEFINE_RG_BUFFER_ACCESS_ARRAY(buffers);

    DEFINE_RG_PARAMETER_ACCESS(textures, buffers);
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

bool ValidatePassDomainHelpers() {
    if (!RGPassHasValidQueueFlags(ERGPassFlags::None) ||
        !RGPassHasQueue(ERGPassFlags::Graphics) ||
        !RGPassHasQueue(ERGPassFlags::Compute) ||
        !RGPassHasQueue(ERGPassFlags::Copy)) {
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

} // namespace

int RunRenderGraphContractFoundationTest() {
    if (!ValidatePassDomainHelpers() || !ValidateRangeOverlapRules()) {
        return 1;
    }

    RenderGraph graph;

    const RenderGraphHandle texture = graph.CreateTexture(
        MOER_TEXT("rg_contract_texture"),
        RGTextureDesc{
            .extent = Extent3D(16, 16, 1),
            .format = PF_R8G8B8A8_UNORM,
            .usage = ETextureUsageFlags::COLOR_ATTACHMENT | ETextureUsageFlags::SAMPLED,
            .mip_levels = 2,
            .array_layers = 1
        }
    );
    const RenderGraphHandle buffer = graph.CreateBuffer(
        MOER_TEXT("rg_contract_buffer"),
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

    auto* access_array_params = graph.Alloc<RGAccessArrayParams>();
    auto* serial_params = graph.Alloc<RGSerialParams>();

    graph.AddSetupPass(MOER_TEXT("PrepareContractFoundation"), [write_params, access_array_params, texture, buffer](RGSetupContext& setup) {
        write_params->prepared_value = 42;
        access_array_params->textures = setup.Graph().Alloc<RGTextureAccessArray>(2);
        access_array_params->buffers = setup.Graph().Alloc<RGBufferAccessArray>(2);

        access_array_params->textures->AddAccess(RGTextureView{
            .handle = texture,
            .range = RGTextureRange{.aspect = ETextureAspectFlags::COLOR, .mip_min = 0, .mip_count = 1},
            .access = ERGAccessMode::Read,
            .state = ETextureState::SHADER_RESOURCE,
            .queue = EQueueType::Graphics,
            .bindless = true
        });
        access_array_params->textures->AddAccess(RGTextureView{
            .handle = texture,
            .range = RGTextureRange{.aspect = ETextureAspectFlags::COLOR, .mip_min = 1, .mip_count = 1},
            .access = ERGAccessMode::Read,
            .queue = EQueueType::Graphics
        }, ETextureState::SHADER_RESOURCE);
        access_array_params->buffers->AddAccess(RGBufferView{
            .handle = buffer,
            .range = RGBufferRange{.offset = 48, .size = 8},
            .access = ERGAccessMode::Read,
            .state = EBufferState::SHADER_RESOURCE,
            .queue = EQueueType::Compute,
            .bindless = true
        });
        access_array_params->buffers->AddAccess(RGBufferView{
            .handle = buffer,
            .range = RGBufferRange{.offset = 96, .size = 16},
            .access = ERGAccessMode::Read,
            .queue = EQueueType::Compute
        }, EBufferState::SHADER_RESOURCE);
    });
    graph.AddPass(MOER_TEXT("WriteContractResources"), write_params, ERGPassFlags::Graphics, [write_params](RHICommandList&, RGContext context) {
        (void)context.Graph();
        if (write_params->prepared_value == 42) {
            ++write_params->execution_count;
        }
    });

    graph.AddPass(MOER_TEXT("SerialContractFence"), serial_params, ERGPassFlags::None, [serial_params](RGContext context) {
        (void)context.Graph();
        ++serial_params->execution_count;
    });

    graph.AddPass(MOER_TEXT("ReadAccessArrays"), access_array_params, ERGPassFlags::Compute, [](RHICommandList&, RGContext) {});

    graph.ExportTexture(texture, ETextureState::SHADER_RESOURCE, EQueueType::Graphics);
    graph.ExportBuffer(buffer, EBufferState::SHADER_RESOURCE, EQueueType::Compute);

    RHICommandList command_list(EQueueType::Graphics);
    graph.Dispatch(&command_list);

    const RGCompiledPlan& plan = graph.GetCompiledPlan();
    if (plan.hazard_edges.size() != 2) {
        LOG_ERROR(MOER_TEXT("RenderGraph foundation expected 2 hazards, got {}"), plan.hazard_edges.size());
        return 1;
    }
    if (!HasHazard(plan, 0, 2, texture, ERGResourceKind::Texture) ||
        !HasHazard(plan, 0, 2, buffer, ERGResourceKind::Buffer)) {
        LOG_ERROR(MOER_TEXT("RenderGraph foundation compiled wrong hazard edges"));
        return 1;
    }
    if (write_params->prepared_value != 42 || write_params->execution_count != 1 ||
        serial_params->execution_count != 1) {
        LOG_ERROR(MOER_TEXT("RenderGraph setup or execution lambda order is invalid"));
        return 1;
    }

    LOG_INFO(MOER_TEXT("RenderGraph contract foundation test passed, hazards={}"), plan.hazard_edges.size());
    return 0;
}

} // namespace Moer::Render::Tests