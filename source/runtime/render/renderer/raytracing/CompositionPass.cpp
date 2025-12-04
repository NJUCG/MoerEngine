#include "CompositionPass.h"

#include "rhi/RHICommand.h"
#include "shader/ShaderResourceManager.h"
#include "shaderheaders/shared/ShaderParameters.h"

namespace Moer::Render::Raytracing {

CompositionPass::CompositionPass(RenderDevice& _device, ShaderManager& _manager, Scene& _scene) :
    device(_device),
    manager(_manager),
    scene(_scene),
    gbuffer_pass_pipeline{manager.Compute<CompositionPassPipeline>("pipelines/raytracing/passes/CompositionPass.hlsl")} {
    gbuffer_constants = device.CreateBuffer<Moer::byte>(
        "CompositionPass::constant_buffer", sizeof(CompositingConstants), EBufferUsageFlags::CONSTANT_BUFFER
    );
}

void CompositionPass::Process(CommandList& _cmd_list, RTContext& _rt_ctx) {

    constants.denoiser_mode  = _rt_ctx.config.denoiser_mode;
    constants.enable_env_map = _rt_ctx.scene_params.enable_env_map ? 1 : 0;
    constants.env_map_handle = _rt_ctx.scene_params.env_map_handle;

    constants.env_rotation = _rt_ctx.scene_params.env_map_rotation;
    constants.env_scale    = _rt_ctx.scene_params.env_map_scale;
    constants.main_view    = _rt_ctx.main_view;
    constants.prev_view    = _rt_ctx.prev_view;

    upload_data.resize(sizeof(CompositingConstants));
    std::memcpy(upload_data.data(), &constants, sizeof(CompositingConstants));

    _cmd_list.CopyFrom(std::move(upload_data), gbuffer_constants->GetView());

    bool        b_current_frame = _rt_ctx.b_current_frame;
    const auto& frame_rt        = _rt_ctx.frame_rt;
    _cmd_list
        .Compute(
            gbuffer_pass_pipeline,
            gbuffer_constants,
            _rt_ctx.frame_rt.hdr_color,
            _rt_ctx.frame_rt.motion,
            b_current_frame ? _rt_ctx.frame_rt.view_depth : _rt_ctx.frame_rt.prev_view_depth,
            b_current_frame ? _rt_ctx.frame_rt.diffuse_albedo : _rt_ctx.frame_rt.prev_diffuse_albedo,
            b_current_frame ? _rt_ctx.frame_rt.specular_roughness : _rt_ctx.frame_rt.prev_specular_roughness,
            b_current_frame ? _rt_ctx.frame_rt.normal : _rt_ctx.frame_rt.prev_normal,
            _rt_ctx.frame_rt.emission,
            _rt_ctx.frame_rt.diffuse_lighting,
            _rt_ctx.frame_rt.specular_lighting,
#if WITH_NRD
            _rt_ctx.frame_rt.denoised_diffuse_lighting,
            _rt_ctx.frame_rt.denoised_specular_lighting,
#else
            _rt_ctx.frame_rt.diffuse_lighting,
            _rt_ctx.frame_rt.specular_lighting,
#endif
            scene.GetBindlessArray()
        )
        .Dispatch(
            uint3(ceil(constants.main_view.rect.x / 8), ceil(constants.main_view.rect.y / 8), 1),
            "CompositionPass"
        );
}
} // namespace Moer::Render::Raytracing