#include "GBufferPass.h"

// 构建当前帧和上一帧的 GBuffer 表面，并打包降噪器属性。

#include "RTResource.h"
#include "shader/ShaderResourceManager.h"
#include "shaderheaders/shared/ShaderParameters.h"

#include <cmath>
#include <cstring>

namespace Moer::Render::Raytracing {

GBufferPass::GBufferPass(RenderDevice& device, ShaderManager& manager, BindlessArrayRef bindless_array) :
    bindless_array(std::move(bindless_array)) {

    gbuffer_constants = device.CreateBuffer<Moer::byte>(
        "Raytracing::gbuffer_constants", sizeof(GBufferConstants), EBufferUsageFlags::CONSTANT_BUFFER
    );
    RTGBufferMacros gbuffer_macros{};
    gbuffer_macros.SetMutation<RaytracingGBufferPipeline::PRINT_TEST>(true);
    gbuffer_pipeline = manager.Compute<RaytracingGBufferPipeline>(
        "pipelines/raytracing/passes/GBufferRT.hlsl", gbuffer_macros
    );

    constexpr int with_nrd = WITH_NRD;
#pragma push_macro("WITH_NRD")
#undef WITH_NRD
    PostProcessGBufferPipeline::MutationSet mutation_set;
    mutation_set.SetMutation<PostProcessGBufferPipeline::WITH_NRD>(with_nrd);
#pragma pop_macro("WITH_NRD")

    gbuffer_postprocess_pipeline = manager.Compute<PostProcessGBufferPipeline>(
        "pipelines/raytracing/passes/PostProcessGBuffer.hlsl", mutation_set
    );
}

void GBufferPass::Process(CommandList& cmd_list, RTContext& rt_ctx) {
    GBufferPassParams                params{};
    const RaytracingBindlessHandles& bindless_handles = rt_ctx.GetBindlessHandles();

    // 场景间接索引表。
    params.instance_buf_hdl  = bindless_handles.instance_buf_hdl;
    params.primitive_buf_hdl = bindless_handles.primitive_buf_hdl;
    params.material_buf_hdl  = bindless_handles.material_buf_hdl;

    // 共享几何数据大缓冲区。
    params.position_buf_hdl       = bindless_handles.position_buf_hdl;
    params.packed_normal_buf_hdl  = bindless_handles.packed_normal_buf_hdl;
    params.packed_tangent_buf_hdl = bindless_handles.packed_tangent_buf_hdl;
    params.texcoord0_buf_hdl      = bindless_handles.texcoord0_buf_hdl;
    params.index_buf_hdl          = bindless_handles.index_buf_hdl;

    // Raytracing 专用的 mesh 级 BLAS 间接索引。
    params.rt_instance_buf_hdl        = bindless_handles.rt_instance_buf_hdl;
    params.rt_primitive_table_buf_hdl = bindless_handles.rt_primitive_table_buf_hdl;

    constants.main_view = rt_ctx.main_view;
    constants.prev_view = rt_ctx.prev_view;
    upload_data.resize(sizeof(GBufferConstants));
    std::memcpy(upload_data.data(), &constants, sizeof(GBufferConstants));

    const FrameResources& frame         = rt_ctx.frame_rt;
    const bool            current_frame = rt_ctx.b_current_frame;
    cmd_list.PushScopeWithTimeScope("GBufferPass");
    cmd_list.CopyFrom(std::move(upload_data), gbuffer_constants->GetView());

    cmd_list
        .Compute(
            gbuffer_pipeline,
            params,
            gbuffer_constants,
            current_frame ? frame.view_depth : frame.prev_view_depth,
            current_frame ? frame.diffuse_albedo : frame.prev_diffuse_albedo,
            current_frame ? frame.specular_roughness : frame.prev_specular_roughness,
            current_frame ? frame.normal : frame.prev_normal,
            frame.emission,
            frame.motion,
            frame.clip_depth,
            rt_ctx.rt_scene->GetTlas(),
            bindless_array
        )
        .Dispatch(
            uint3(ceil(constants.main_view.rect.x / 16), ceil(constants.main_view.rect.y / 16), 1),
            "RaytracingGbuffer"
        );

    cmd_list
        .Compute(
            gbuffer_postprocess_pipeline,
            current_frame ? frame.specular_roughness : frame.prev_specular_roughness,
            frame.normal_roughness,
            current_frame ? frame.normal : frame.prev_normal,
            current_frame ? frame.view_depth : frame.prev_view_depth
        )
        .Dispatch(
            uint3(ceil(constants.main_view.rect.x / 16), ceil(constants.main_view.rect.y / 16), 1),
            "PostProcessGBuffer"
        );

    cmd_list.PopScopeWithTimeScope();
}

} // namespace Moer::Render::Raytracing
