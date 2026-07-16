#include "VisualizePass.h"

// 将选定的调试视图写入渲染器的调试输出纹理。

#include "RTResource.h"
#include "rhi/RHICommand.h"
#include "rhi/RHIResource.h"
#include "shader/ShaderResourceManager.h"

namespace Moer::Render::Raytracing {

VisualizePass::VisualizePass(RenderDevice& device, ShaderManager& manager) {
    visualize_pipeline =
        manager.Compute<VisualizePipeline>("pipelines/raytracing/passes/VisualizePass.hlsl");
    visualize_params_buffer = device.CreateBuffer<Moer::byte>(
        "Raytracing::VisualizeBuffer", sizeof(VisualizeParams), EBufferUsageFlags::CONSTANT_BUFFER
    );
}

void VisualizePass::Process(
    CommandList&           cmd_list,
    RTContext&             rt_ctx,
    const VisualizeConfig& config,
    BindlessArrayRef       bindless_array
) {
    params.grid_params      = rt_ctx.is_ctx.GetGridParams();
    params.b_split          = config.b_split;
    params.split_ratio      = config.split_ratio;
    params.visualize_mode   = config.visualize_mode;
    params.main_view        = rt_ctx.main_view;
    params.bindless_handles = rt_ctx.GetBindlessHandles();
    params.output_size      = rt_ctx.frame_rt.ldr_color->GetExtent().xy;

    cmd_list.CopyFrom(
        std::span<Moer::byte>((Moer::byte*)&params, sizeof(VisualizeParams)),
        visualize_params_buffer->GetView()
    );

    const auto div_ceil = [](uint value, uint divisor) -> uint {
        return (value + divisor - 1) / divisor;
    };
    const FrameResources& frame = rt_ctx.frame_rt;
    cmd_list
        .Compute(
            visualize_pipeline,
            visualize_params_buffer,
            frame.ldr_color,
            frame.diffuse_lighting,
            frame.specular_lighting,
            rt_ctx.b_current_frame ? frame.view_depth : frame.prev_view_depth,
            frame.emission,
            frame.debug_color,
            bindless_array
        )
        .Dispatch(
            uint3(div_ceil(params.output_size.x, 16), div_ceil(params.output_size.y, 16), 1), "Visualize"
        );
}
} // namespace Moer::Render::Raytracing
