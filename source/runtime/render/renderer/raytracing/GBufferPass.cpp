#include "GBufferPass.h"

#include "scene/Scene.h"
#include "shader/ShaderResourceManager.h"
#include "shaderheaders/shared/ShaderParameters.h"

namespace Moer::Render::Raytracing {

namespace {

struct RGGBufferUploadParams {
    DEFINE_RG_BUFFER_ACCESS(constants, EBufferState::TRANSFER_DST);
    DEFINE_RG_PARAMETER_ACCESS(constants);
};

struct RGGBufferTraceParams {
    DEFINE_RG_BUFFER_ACCESS(constants, EBufferState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(view_depth, ETextureState::UNORDERED_ACCESS);
    DEFINE_RG_TEXTURE_ACCESS(diffuse_albedo, ETextureState::UNORDERED_ACCESS);
    DEFINE_RG_TEXTURE_ACCESS(specular_roughness, ETextureState::UNORDERED_ACCESS);
    DEFINE_RG_TEXTURE_ACCESS(normal, ETextureState::UNORDERED_ACCESS);
    DEFINE_RG_TEXTURE_ACCESS(emission, ETextureState::UNORDERED_ACCESS);
    DEFINE_RG_TEXTURE_ACCESS(motion, ETextureState::UNORDERED_ACCESS);
    DEFINE_RG_TEXTURE_ACCESS(clip_depth, ETextureState::UNORDERED_ACCESS);
    DEFINE_RG_BUFFER_ACCESS(tlas_buffer, EBufferState::ACCELERATION_STRUCTURE_READ);
    DEFINE_RG_PARAMETER_ACCESS(
        constants,
        view_depth,
        diffuse_albedo,
        specular_roughness,
        normal,
        emission,
        motion,
        clip_depth,
        tlas_buffer
    );
};

struct RGGBufferPostProcessParams {
    DEFINE_RG_TEXTURE_ACCESS(specular_roughness, ETextureState::UNORDERED_ACCESS);
    DEFINE_RG_TEXTURE_ACCESS(normal_roughness, ETextureState::UNORDERED_ACCESS);
    DEFINE_RG_TEXTURE_ACCESS(normal, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(view_depth, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_PARAMETER_ACCESS(specular_roughness, normal_roughness, normal, view_depth);
};

} // namespace

GBufferPass::GBufferPass(RenderDevice& _device, ShaderManager& _manager) :
    device(_device),
    manager(_manager) {

    gbuffer_constants = device.CreateBuffer<Moer::byte>(
        MOER_TEXT("Raytracing::gbuffer_constants"), sizeof(GBufferConstants), EBufferUsageFlags::CONSTANT_BUFFER
    );
    RTGBufferMacros gbuffer_macros{};
    gbuffer_macros.SetMutation<RaytracingGBufferPipeline::PRINT_TEST>(true);
    gbuffer_pass_pipeline = std::move(manager.Compute<RaytracingGBufferPipeline>(
        "pipelines/raytracing/passes/GBufferRT.hlsl", gbuffer_macros
    ));

    int with_nrd = WITH_NRD;
#pragma push_macro("WITH_NRD")
#undef WITH_NRD
    PostProcessGBufferPipeline::MutationSet mutation_set;
    mutation_set.SetMutation<PostProcessGBufferPipeline::WITH_NRD>(with_nrd);
#pragma pop_macro("WITH_NRD")

    post_process_pipeline = std::move(manager.Compute<PostProcessGBufferPipeline>(
        "pipelines/raytracing/passes/PostProcessGBuffer.hlsl", mutation_set
    ));
}

GBufferPass::PreparedCommand GBufferPass::Prepare(const RTContext& _rt_ctx) const {
    PreparedCommand           command{};
    RaytracingBindlessHandles bindless_handles = _rt_ctx.GetBindlessHandles();

    // 鍦烘櫙缁撴瀯鏁版嵁
    command.params.instance_buf_hdl  = bindless_handles.instance_buf_hdl;
    command.params.primitive_buf_hdl = bindless_handles.primitive_buf_hdl;
    command.params.material_buf_hdl  = bindless_handles.material_buf_hdl;

    // MegaBuffers
    command.params.position_buf_hdl       = bindless_handles.position_buf_hdl;
    command.params.packed_normal_buf_hdl  = bindless_handles.packed_normal_buf_hdl;
    command.params.packed_tangent_buf_hdl = bindless_handles.packed_tangent_buf_hdl;
    command.params.texcoord0_buf_hdl      = bindless_handles.texcoord0_buf_hdl;
    command.params.index_buf_hdl          = bindless_handles.index_buf_hdl;
    command.params.debug_normal_sample    = 0u;

    command.constants.main_view = _rt_ctx.main_view;
    command.constants.prev_view = _rt_ctx.prev_view;
    command.dispatch_groups     = uint3(
        ceil(command.constants.main_view.rect.x / 16),
        ceil(command.constants.main_view.rect.y / 16),
        1
    );
    return command;
}

GBufferPass::RecordResources GBufferPass::CaptureResources(const RTContext& _rt_ctx) {
    const FrameResources& frame_rt        = _rt_ctx.frame_rt;
    bool                  b_current_frame = _rt_ctx.b_current_frame;
    return RecordResources{
        .view_depth         = RTRHI(b_current_frame ? frame_rt.view_depth : frame_rt.prev_view_depth),
        .diffuse_albedo     = RTRHI(b_current_frame ? frame_rt.diffuse_albedo : frame_rt.prev_diffuse_albedo),
        .specular_roughness = RTRHI(b_current_frame ? frame_rt.specular_roughness : frame_rt.prev_specular_roughness),
        .normal             = RTRHI(b_current_frame ? frame_rt.normal : frame_rt.prev_normal),
        .emission           = RTRHI(frame_rt.emission),
        .motion             = RTRHI(frame_rt.motion),
        .clip_depth         = RTRHI(frame_rt.clip_depth),
        .normal_roughness   = RTRHI(frame_rt.normal_roughness),
        .tlas               = _rt_ctx.rt_scene ? _rt_ctx.rt_scene->GetTlas() : RaytracingTlasRef{},
        .bindless_array     = _rt_ctx.GetBindlessArray()
    };
}

void GBufferPass::RecordConstantsUpload(CommandList& _cmd_list, const PreparedCommand& _command) {
    Array<byte> upload_data(sizeof(GBufferConstants));
    upload_data.assign((byte*)&_command.constants, (byte*)&_command.constants + sizeof(GBufferConstants));

    _cmd_list.CopyFrom(std::move(upload_data), gbuffer_constants->GetView());
}

void GBufferPass::RecordTraceGBuffer(
    CommandList&           _cmd_list,
    const PreparedCommand& _command,
    const RecordResources& _resources
) {
    _cmd_list
        .Compute(
            gbuffer_pass_pipeline,
            _command.params,
            gbuffer_constants,
            _resources.view_depth,
            _resources.diffuse_albedo,
            _resources.specular_roughness,
            _resources.normal,
            _resources.emission,
            _resources.motion,
            _resources.clip_depth,
            _resources.tlas,
            _resources.bindless_array
        )
        .Dispatch(_command.dispatch_groups, MOER_TEXT("RaytracingGbuffer"));
}

void GBufferPass::RecordPostProcessGBuffer(
    CommandList&           _cmd_list,
    const PreparedCommand& _command,
    const RecordResources& _resources
) {
    _cmd_list
        .Compute(
            post_process_pipeline,
            _resources.specular_roughness,
            _resources.normal_roughness,
            _resources.normal,
            _resources.view_depth
        )
        .Dispatch(_command.dispatch_groups, MOER_TEXT("PostProcessGBuffer"));
}

void GBufferPass::AddPasses(RenderGraph& _graph, const RTGraphFrameResources& _rg, const RTContext& _rt_ctx) {
    auto* command   = _graph.Alloc<PreparedCommand>(Prepare(_rt_ctx));
    auto* resources = _graph.Alloc<RecordResources>(CaptureResources(_rt_ctx));
    RGBuffer* constants = _graph.ImportBuffer(MOER_TEXT("RT.GBuffer.constants"), gbuffer_constants, EQueueType::Graphics);
    RGBuffer* tlas_buffer = ImportRTTlasBufferIfValid(_graph, MOER_TEXT("RT.GBuffer.tlas_buffer"), resources->tlas);

    auto* upload_params      = _graph.Alloc<RGGBufferUploadParams>();
    upload_params->constants = RGBufferView{.buffer = constants};
    _graph.AddPass(
        MOER_TEXT("RT.GBuffer.UploadConstants"),
        upload_params,
        ERGPassFlags::Graphics,
        [this, command](RHICommandList& cmd_list, RGContext) {
            RecordConstantsUpload(cmd_list, *command);
        }
    );

    auto* trace_params                = _graph.Alloc<RGGBufferTraceParams>();
    trace_params->constants           = RGBufferView{.buffer = constants};
    trace_params->view_depth          = RTWholeTextureView(_rg.current_view_depth);
    trace_params->diffuse_albedo      = RTWholeTextureView(_rg.current_diffuse_albedo);
    trace_params->specular_roughness  = RTWholeTextureView(_rg.current_specular_roughness);
    trace_params->normal              = RTWholeTextureView(_rg.current_normal);
    trace_params->emission            = RTWholeTextureView(_rg.emission);
    trace_params->motion              = RTWholeTextureView(_rg.motion);
    trace_params->clip_depth          = RTWholeTextureView(_rg.clip_depth);
    trace_params->tlas_buffer         = RGBufferView{.buffer = tlas_buffer};
    _graph.AddPass(
        MOER_TEXT("RT.GBuffer.Trace"),
        trace_params,
        s_rt_graph_graphics_compute_pass,
        [this, command, resources](RHICommandList& cmd_list, RGContext) {
            RecordTraceGBuffer(cmd_list, *command, *resources);
        }
    );

    auto* post_params               = _graph.Alloc<RGGBufferPostProcessParams>();
    post_params->specular_roughness = RTWholeTextureView(_rg.current_specular_roughness);
    post_params->normal_roughness   = RTWholeTextureView(_rg.normal_roughness);
    post_params->normal             = RTWholeTextureView(_rg.current_normal);
    post_params->view_depth         = RTWholeTextureView(_rg.current_view_depth);
    _graph.AddPass(
        MOER_TEXT("RT.GBuffer.PostProcess"),
        post_params,
        s_rt_graph_graphics_compute_pass,
        [this, command, resources](RHICommandList& cmd_list, RGContext) {
            RecordPostProcessGBuffer(cmd_list, *command, *resources);
        }
    );
}

} // namespace Moer::Render::Raytracing
