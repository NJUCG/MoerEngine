#include "VisualizePass.h"

#include "RTResource.h"
#include "rhi/RHICommand.h"
#include "rhi/RHIResource.h"
#include "shader/ShaderResourceManager.h"

namespace Moer::Render::Raytracing {

VisualizePass::VisualizePass(RenderDevice& _device, ShaderManager& _manager) :
    device(_device),
    manager(_manager) {

    visualize_pipeline =
        _manager.Compute<VisualizePipeline>("pipelines/raytracing/passes/VisualizePass.hlsl");
    visualize_params_buffer = device.CreateBuffer<Moer::byte>(
        MOER_TEXT("Raytracing::VisualizeBuffer"), sizeof(VisualizeParams), EBufferUsageFlags::CONSTANT_BUFFER
    );
}

void VisualizePass::Process(
    CommandList&           _cmdlist,
    RTContext&             _ctx,
    const VisualizeConfig& _cfg,
    BindlessArrayRef       _bdls_array
) {

    params.grid_params      = _ctx.is_ctx.GetGridParams();
    params.b_split          = _cfg.b_split;
    params.split_ratio      = _cfg.split_ratio;
    params.visualize_mode   = _cfg.visualize_mode;
    params.main_view        = _ctx.main_view;
    params.bindless_handles = _ctx.GetBindlessHandles();
    params.output_size      = _ctx.frame_rt.ldr_color->GetExtent().xy;

    _cmdlist.CopyFrom(
        std::span<Moer::byte>((Moer::byte*)&params, sizeof(VisualizeParams)),
        visualize_params_buffer->GetView()
    );

    auto div_ceil = [](uint _a, uint _b) -> uint {
        return (_a + _b - 1) / _b;
    };
    _cmdlist
        .Compute(
            visualize_pipeline,
            visualize_params_buffer,
            _ctx.frame_rt.ldr_color,
            _ctx.frame_rt.diffuse_lighting,
            _ctx.frame_rt.specular_lighting,
            _ctx.b_current_frame ? _ctx.frame_rt.view_depth : _ctx.frame_rt.prev_view_depth,
            _ctx.frame_rt.emission,
            _ctx.frame_rt.debug_color,
            _bdls_array
        )
        .Dispatch(
            uint3(div_ceil(params.output_size.x, 16), div_ceil(params.output_size.y, 16), 1),
            MOER_TEXT("Visualize")
        );
}
} // namespace Moer::Render::Raytracing