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

    void GBufferPass::Process(CommandList& _cmd_list, RTContext& _rt_ctx) {
        GBufferPassParams         params{};
        RaytracingBindlessHandles bindless_handles = _rt_ctx.GetBindlessHandles();
        params.geometry_data_handle                = bindless_handles.geom_data;
        params.instance_data_handle                = bindless_handles.instance_data;
        params.material_data_handle                = bindless_handles.material_data;

        Entity main_cam_entity = scene.GetMainCamera();

        constants.main_view = constants.main_view;
        constants.prev_view = constants.prev_view;

        FrameResources& gbuffer_res = _rt_ctx.frame_rt;

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