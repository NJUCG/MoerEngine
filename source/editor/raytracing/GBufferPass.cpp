#include "GBufferPass.h"

#include "RTResource.h"
#include "scene/CameraManager.h"
#include "scene/RenderableManager.h"
#include "scene/Scene.h"
#include "shader/ShaderResourceManager.h"
#include "shaderheaders/shared/ShaderParameters.h"

namespace Moer::Render::Raytracing {

GBufferPass::GBufferPass(RenderDevice& _device, ShaderManager& _manager, Scene& _scene) :
    device(_device),
    manager(_manager),
    scene(_scene),
    gbuffer_pass_pipeline{manager.Compute<RaytracingGBufferPipeline>("hwrt/GBufferRT.hlsl")},
    post_process_pipeline{manager.Compute<PostProcessGBufferPipeline>("hwrt/PostProcessGBuffer.hlsl")} {

    gbuffer_constants =
        device.CreateBuffer<Moer::byte>(sizeof(GBufferConstants), EBufferUsageFlags::CONSTANT_BUFFER);
    gbuffer_constants->SetName("gbuffer_constants");
}

void GBufferPass::Process(CommandList& _cmd_list, RTContext& _rt_ctx) {
    GBufferPassParams         params{};
    RaytracingBindlessHandles bindless_handles = _rt_ctx.GetBindlessHandles();
    params.geometry_data_handle                = bindless_handles.geom_data;
    params.instance_data_handle                = bindless_handles.instance_data;
    params.material_data_handle                = bindless_handles.material_data;

    Entity main_cam_entity = scene.GetMainCamera();

    constants.main_view = _rt_ctx.main_view;
    constants.prev_view = _rt_ctx.prev_view;
    upload_data.resize(sizeof(GBufferConstants));
    std::memcpy(upload_data.data(), &constants, sizeof(GBufferConstants));

    FrameResources& frame_rt        = _rt_ctx.frame_rt;
    bool            b_current_frame = _rt_ctx.b_current_frame;
    _cmd_list.PushScope("GBufferPass");
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
            scene.GetBindlessArray()
        )
        .Dispatch(
            uint3(ceil(constants.main_view.rect.x / 16), ceil(constants.main_view.rect.y / 16), 1),
            "RaytracingGbuffer"
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
            "PostProcessGBuffer"
        );

    _cmd_list.PopScope();
}
} // namespace Moer::Render::Raytracing