#include "GBufferPass.h"

#include "RTResource.h"
#include "scene/Scene.h"
#include "shader/ShaderResourceManager.h"
#include "shaderheaders/shared/ShaderParameters.h"

namespace Moer::Render::Raytracing {

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

void GBufferPass::Process(CommandList& _cmd_list, RTContext& _rt_ctx) {
    GBufferPassParams         params{};
    RaytracingBindlessHandles bindless_handles = _rt_ctx.GetBindlessHandles();

    // 鍦烘櫙缁撴瀯鏁版嵁
    params.instance_buf_hdl  = bindless_handles.instance_buf_hdl;
    params.primitive_buf_hdl = bindless_handles.primitive_buf_hdl;
    params.material_buf_hdl  = bindless_handles.material_buf_hdl;

    // MegaBuffers
    params.position_buf_hdl       = bindless_handles.position_buf_hdl;
    params.packed_normal_buf_hdl  = bindless_handles.packed_normal_buf_hdl;
    params.packed_tangent_buf_hdl = bindless_handles.packed_tangent_buf_hdl;
    params.texcoord0_buf_hdl      = bindless_handles.texcoord0_buf_hdl;
    params.index_buf_hdl          = bindless_handles.index_buf_hdl;

    constants.main_view = _rt_ctx.main_view;
    constants.prev_view = _rt_ctx.prev_view;
    upload_data.resize(sizeof(GBufferConstants));
    std::memcpy(upload_data.data(), &constants, sizeof(GBufferConstants));

    FrameResources& frame_rt        = _rt_ctx.frame_rt;
    bool            b_current_frame = _rt_ctx.b_current_frame;
    _cmd_list.PushScopeWithTimeScope(MOER_TEXT("GBufferPass"));
    _cmd_list.CopyFrom(std::move(upload_data), gbuffer_constants->GetView());

    _cmd_list
        .Compute(
            gbuffer_pass_pipeline,
            params,
            gbuffer_constants,
            b_current_frame ? frame_rt.view_depth : frame_rt.prev_view_depth,
            b_current_frame ? frame_rt.diffuse_albedo : frame_rt.prev_diffuse_albedo,
            b_current_frame ? frame_rt.specular_roughness : frame_rt.prev_specular_roughness,
            b_current_frame ? frame_rt.normal : frame_rt.prev_normal,
            frame_rt.emission,
            frame_rt.motion,
            frame_rt.clip_depth,
            _rt_ctx.rt_scene->GetTlas(),
            _rt_ctx.GetBindlessArray()
        )
        .Dispatch(
            uint3(ceil(constants.main_view.rect.x / 16), ceil(constants.main_view.rect.y / 16), 1),
            MOER_TEXT("RaytracingGbuffer")
        );

    _cmd_list
        .Compute(
            post_process_pipeline,
            b_current_frame ? frame_rt.specular_roughness : frame_rt.prev_specular_roughness,
            frame_rt.normal_roughness,
            b_current_frame ? frame_rt.normal : frame_rt.prev_normal,
            b_current_frame ? frame_rt.view_depth : frame_rt.prev_view_depth
        )
        .Dispatch(
            uint3(ceil(constants.main_view.rect.x / 16), ceil(constants.main_view.rect.y / 16), 1),
            MOER_TEXT("PostProcessGBuffer")
        );

    _cmd_list.PopScopeWithTimeScope();
}
} // namespace Moer::Render::Raytracing