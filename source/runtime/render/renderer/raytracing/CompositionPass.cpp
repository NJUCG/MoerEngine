#include "CompositionPass.h"

#include "rhi/RHICommand.h"
#include "shader/ShaderResourceManager.h"
#include "shaderheaders/shared/ShaderParameters.h"

namespace Moer::Render::Raytracing {

namespace {

struct RGCompositionUploadParams {
    DEFINE_RG_BUFFER_ACCESS(constants, EBufferState::TRANSFER_DST);
    DEFINE_RG_PARAMETER_ACCESS(constants);
};

struct RGCompositionDispatchParams {
    DEFINE_RG_BUFFER_ACCESS(constants, EBufferState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(hdr_color, ETextureState::UNORDERED_ACCESS);
    DEFINE_RG_TEXTURE_ACCESS(motion, ETextureState::UNORDERED_ACCESS);
    DEFINE_RG_TEXTURE_ACCESS(view_depth, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(diffuse_albedo, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(specular_roughness, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(normal, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(emission, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(diffuse_lighting, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(specular_lighting, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(denoised_diffuse_lighting, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(denoised_specular_lighting, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_PARAMETER_ACCESS(
        constants,
        hdr_color,
        motion,
        view_depth,
        diffuse_albedo,
        specular_roughness,
        normal,
        emission,
        diffuse_lighting,
        specular_lighting,
        denoised_diffuse_lighting,
        denoised_specular_lighting
    );
};

} // namespace

CompositionPass::CompositionPass(RenderDevice& _device, ShaderManager& _manager) :
    device(_device),
    manager(_manager) {

    int with_nrd = WITH_NRD;
#pragma push_macro("WITH_NRD")
#undef WITH_NRD
    CompositionPassPipeline::MutationSet mutation_set;
    mutation_set.SetMutation<CompositionPassPipeline::WITH_NRD>(with_nrd);
#pragma pop_macro("WITH_NRD")

    gbuffer_pass_pipeline = std::move(manager.Compute<CompositionPassPipeline>(
        "pipelines/raytracing/passes/CompositionPass.hlsl", mutation_set
    ));

    gbuffer_constants = device.CreateBuffer<Moer::byte>(
        MOER_TEXT("CompositionPass::constant_buffer"), sizeof(CompositingConstants), EBufferUsageFlags::CONSTANT_BUFFER
    );
}

CompositionPass::PreparedCommand CompositionPass::Prepare(const RTContext& _rt_ctx) const {
    PreparedCommand command{};

    command.constants.denoiser_mode  = _rt_ctx.config.denoiser_mode;
    command.constants.enable_env_map = _rt_ctx.scene_params.enable_env_map ? 1 : 0;
    command.constants.env_map_handle = _rt_ctx.scene_params.env_map_handle;

    command.constants.env_rotation = _rt_ctx.scene_params.env_map_rotation;
    command.constants.env_scale    = _rt_ctx.scene_params.env_map_scale;
    command.constants.main_view    = _rt_ctx.main_view;
    command.constants.prev_view    = _rt_ctx.prev_view;
    command.dispatch_groups        = uint3(
        ceil(command.constants.main_view.rect.x / 8),
        ceil(command.constants.main_view.rect.y / 8),
        1
    );
    return command;
}

CompositionPass::RecordResources CompositionPass::CaptureResources(const RTContext& _rt_ctx) {
    bool        b_current_frame = _rt_ctx.b_current_frame;
    const auto& frame_rt        = _rt_ctx.frame_rt;
    return RecordResources{
        .hdr_color                   = frame_rt.hdr_color,
        .motion                      = frame_rt.motion,
        .view_depth                  = b_current_frame ? frame_rt.view_depth : frame_rt.prev_view_depth,
        .diffuse_albedo              = b_current_frame ? frame_rt.diffuse_albedo : frame_rt.prev_diffuse_albedo,
        .specular_roughness          = b_current_frame ? frame_rt.specular_roughness : frame_rt.prev_specular_roughness,
        .normal                      = b_current_frame ? frame_rt.normal : frame_rt.prev_normal,
        .emission                    = frame_rt.emission,
        .diffuse_lighting            = frame_rt.diffuse_lighting,
        .specular_lighting           = frame_rt.specular_lighting,
#if WITH_NRD
        .denoised_diffuse_lighting   = frame_rt.denoised_diffuse_lighting,
        .denoised_specular_lighting  = frame_rt.denoised_specular_lighting,
#else
        .denoised_diffuse_lighting   = frame_rt.diffuse_lighting,
        .denoised_specular_lighting  = frame_rt.specular_lighting,
#endif
        .bindless_array              = _rt_ctx.GetBindlessArray()
    };
}

void CompositionPass::RecordConstantsUpload(CommandList& _cmd_list, const PreparedCommand& _command) {
    Array<byte> upload_data(sizeof(CompositingConstants));
    upload_data.assign((byte*)&_command.constants, (byte*)&_command.constants + sizeof(CompositingConstants));
    _cmd_list.CopyFrom(std::move(upload_data), gbuffer_constants->GetView());
}

void CompositionPass::RecordComposition(
    CommandList&           _cmd_list,
    const PreparedCommand& _command,
    const RecordResources& _resources
) {
    _cmd_list
        .Compute(
            gbuffer_pass_pipeline,
            gbuffer_constants,
            _resources.hdr_color,
            _resources.motion,
            _resources.view_depth,
            _resources.diffuse_albedo,
            _resources.specular_roughness,
            _resources.normal,
            _resources.emission,
            _resources.diffuse_lighting,
            _resources.specular_lighting,
            _resources.denoised_diffuse_lighting,
            _resources.denoised_specular_lighting,
            _resources.bindless_array
        )
        .Dispatch(_command.dispatch_groups, MOER_TEXT("CompositionPass"));
}

void CompositionPass::AddPass(RenderGraph& _graph, const RTGraphFrameResources& _rg, const RTContext& _rt_ctx) {
    auto* command   = _graph.Alloc<PreparedCommand>(Prepare(_rt_ctx));
    auto* resources = _graph.Alloc<RecordResources>(CaptureResources(_rt_ctx));
    RGBuffer* constants = _graph.ImportBuffer(MOER_TEXT("RT.Composition.constants"), gbuffer_constants, EQueueType::Graphics);

    auto* upload_params      = _graph.Alloc<RGCompositionUploadParams>();
    upload_params->constants = RGBufferView{.buffer = constants};
    _graph.AddPass(
        MOER_TEXT("RT.Composition.UploadConstants"),
        upload_params,
        ERGPassFlags::Graphics,
        [this, command](RHICommandList& cmd_list, RGContext) {
            RecordConstantsUpload(cmd_list, *command);
        }
    );

    auto* dispatch_params                         = _graph.Alloc<RGCompositionDispatchParams>();
    dispatch_params->constants                    = RGBufferView{.buffer = constants};
    dispatch_params->hdr_color                    = RTWholeTextureView(_rg.hdr_color);
    dispatch_params->motion                       = RTWholeTextureView(_rg.motion);
    dispatch_params->view_depth                   = RTWholeTextureView(_rg.current_view_depth);
    dispatch_params->diffuse_albedo               = RTWholeTextureView(_rg.current_diffuse_albedo);
    dispatch_params->specular_roughness           = RTWholeTextureView(_rg.current_specular_roughness);
    dispatch_params->normal                       = RTWholeTextureView(_rg.current_normal);
    dispatch_params->emission                     = RTWholeTextureView(_rg.emission);
    dispatch_params->diffuse_lighting             = RTWholeTextureView(_rg.diffuse_lighting);
    dispatch_params->specular_lighting            = RTWholeTextureView(_rg.specular_lighting);
#if WITH_NRD
    dispatch_params->denoised_diffuse_lighting    = RTWholeTextureView(_rg.denoised_diffuse_lighting);
    dispatch_params->denoised_specular_lighting   = RTWholeTextureView(_rg.denoised_specular_lighting);
#else
    dispatch_params->denoised_diffuse_lighting    = RTWholeTextureView(_rg.diffuse_lighting);
    dispatch_params->denoised_specular_lighting   = RTWholeTextureView(_rg.specular_lighting);
#endif
    _graph.AddPass(
        MOER_TEXT("RT.Composition.Dispatch"),
        dispatch_params,
        s_rt_graph_graphics_compute_pass,
        [this, command, resources](RHICommandList& cmd_list, RGContext) {
            RecordComposition(cmd_list, *command, *resources);
        }
    );
}
} // namespace Moer::Render::Raytracing