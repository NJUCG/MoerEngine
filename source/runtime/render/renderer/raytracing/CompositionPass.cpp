#include "CompositionPass.h"

// Selects the active GBuffer history and composes the HDR lighting result.

#include "rhi/RHICommand.h"
#include "shader/ShaderResourceManager.h"
#include "shaderheaders/shared/ShaderParameters.h"

#include <cmath>
#include <cstring>

namespace Moer::Render::Raytracing {

CompositionPass::CompositionPass(
    RenderDevice&    device,
    ShaderManager&   manager,
    BindlessArrayRef bindless_array
) : bindless_array(std::move(bindless_array)) {

    constexpr int with_nrd = WITH_NRD;
#pragma push_macro("WITH_NRD")
#undef WITH_NRD
    CompositionPassPipeline::MutationSet mutation_set;
    mutation_set.SetMutation<CompositionPassPipeline::WITH_NRD>(with_nrd);
#pragma pop_macro("WITH_NRD")

    composition_pipeline = manager.Compute<CompositionPassPipeline>(
        "pipelines/raytracing/passes/CompositionPass.hlsl", mutation_set
    );

    composition_constants = device.CreateBuffer<Moer::byte>(
        "CompositionPass::constant_buffer", sizeof(CompositingConstants), EBufferUsageFlags::CONSTANT_BUFFER
    );
}

void CompositionPass::Process(CommandList& cmd_list, RTContext& rt_ctx) {
    constants.denoiser_mode  = rt_ctx.config.denoiser_mode;
    constants.enable_env_map = rt_ctx.scene_params.enable_env_map ? 1 : 0;
    constants.env_map_handle = rt_ctx.scene_params.env_map_handle;
    constants.env_rotation   = rt_ctx.scene_params.env_map_rotation;
    constants.env_scale      = rt_ctx.scene_params.env_map_scale;
    constants.main_view      = rt_ctx.main_view;
    constants.prev_view      = rt_ctx.prev_view;

    upload_data.resize(sizeof(CompositingConstants));
    std::memcpy(upload_data.data(), &constants, sizeof(CompositingConstants));
    cmd_list.CopyFrom(std::move(upload_data), composition_constants->GetView());

    const bool            current_frame = rt_ctx.b_current_frame;
    const FrameResources& frame          = rt_ctx.frame_rt;
    cmd_list
        .Compute(
            composition_pipeline,
            composition_constants,
            frame.hdr_color,
            frame.motion,
            current_frame ? frame.view_depth : frame.prev_view_depth,
            current_frame ? frame.diffuse_albedo : frame.prev_diffuse_albedo,
            current_frame ? frame.specular_roughness : frame.prev_specular_roughness,
            current_frame ? frame.normal : frame.prev_normal,
            frame.emission,
            frame.diffuse_lighting,
            frame.specular_lighting,
#if WITH_NRD
            frame.denoised_diffuse_lighting,
            frame.denoised_specular_lighting,
#else
            frame.diffuse_lighting,
            frame.specular_lighting,
#endif
            bindless_array
        )
        .Dispatch(
            uint3(ceil(constants.main_view.rect.x / 8), ceil(constants.main_view.rect.y / 8), 1),
            "CompositionPass"
        );
}

} // namespace Moer::Render::Raytracing
