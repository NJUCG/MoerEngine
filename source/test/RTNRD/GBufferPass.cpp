#include "GBufferPass.h"
#include "RTResource.h"
#include "scene/CameraManager.h"
#include "scene/RenderableManager.h"
#include "shader/ShaderResourceManager.h"
#include "shaderheaders/shared/ShaderParameters.h"
#include "scene/Scene.h"
namespace Moer::Render {
    GBufferPass::GBufferPass(RenderDevice& _device, ShaderManager& _manager, Scene& _scene)
        : device(_device), manager(_manager), scene(_scene), gbuffer_pass_pipeline{manager.Compute<RaytracingGBufferPipeline>("hwrt/GBufferRT.hlsl")} {
        gbuffer_constants = device.CreateBuffer<Moer::byte>(sizeof(GBufferConstants), EBufferUsageFlags::CONSTANT_BUFFER);
        gbuffer_constants->SetName("gbuffer_constants");
    }

    void GBufferPass::PreTickCamera() {
        constants.prev_view = constants.main_view;
    }

    void GBufferPass::UpdateMainView(uint2 _rect) {
        Entity main_cam_entity = scene.GetMainCamera();

        auto main_cam                  = CameraManager::Get().Get(main_cam_entity);
        constants.main_view.view2world = Transpose(main_cam->GetToWorldMatrix());
        constants.main_view.world2view = Transpose(main_cam->GetViewMatrix());
        constants.main_view.world2clip = Transpose(main_cam->GetProjectionMatrix() * main_cam->GetViewMatrix());
        constants.main_view.view2clip  = Transpose(main_cam->GetProjectionMatrix());
        constants.main_view.clip2view  = Inverse(constants.main_view.view2clip);
        constants.main_view.clip2world = Inverse(constants.main_view.world2clip);
        constants.main_view.frustum    = main_cam->GetFrustum();
        constants.main_view.near_far   = float2(main_cam->GetNearClip(), main_cam->GetFarClip());
        constants.main_view.rect       = float2(_rect);
        constants.main_view.inv_rect   = float2(1.f / constants.main_view.rect.x, 1.f / constants.main_view.rect.y);
        constants.main_view.dir_or_pos = float4(main_cam->GetPosition(), 1.f);
    }

    void GBufferPass::Process(CommandList& _cmd_list, RTContext& _rt_ctx) {
        GBufferPassParams params{};
        params.geometry_data_handle = _rt_ctx.geom_data_buf_handle;
        params.instance_data_handle = _rt_ctx.instance_data_buf_handle;
        params.material_data_handle = _rt_ctx.material_data_buf_handle;

        Entity main_cam_entity = scene.GetMainCamera();

        UpdateMainView(_rt_ctx.gbuffer_res.view_depth->GetExtent().xy);

        GBufferResources& gbuffer_res = _rt_ctx.gbuffer_res;

        _cmd_list.CopyFrom(std::span<Moer::byte>((Moer::byte*)&constants, sizeof(GBufferConstants)), gbuffer_constants->GetView());

        _cmd_list.Compute(gbuffer_pass_pipeline,
                          params,
                          gbuffer_constants,
                          gbuffer_res.view_depth,
                          gbuffer_res.diffuse_albedo,
                          gbuffer_res.specular_roughness,
                          gbuffer_res.normal,
                          gbuffer_res.emission,
                          gbuffer_res.motion,
                          gbuffer_res.clip_depth,
                          _rt_ctx.rt_scene->GetTlas(),
                          scene.GetBindlessArray())
            .Dispatch(uint3(ceil(constants.main_view.rect.x / 16), ceil(constants.main_view.rect.y / 16), 1));
    }
}// namespace Moer::Render