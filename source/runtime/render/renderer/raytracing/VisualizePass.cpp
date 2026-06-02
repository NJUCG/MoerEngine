#include "VisualizePass.h"

#include "rhi/RHICommand.h"
#include "rhi/RHIResource.h"
#include "shader/ShaderResourceManager.h"

namespace Moer::Render::Raytracing {

namespace {

struct RGVisualizeUploadParams {
    DEFINE_RG_BUFFER_ACCESS(constants, EBufferState::TRANSFER_DST);
    DEFINE_RG_PARAMETER_ACCESS(constants);
};

struct RGVisualizeDispatchParams {
    DEFINE_RG_BUFFER_ACCESS(constants, EBufferState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(ldr_color, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(diffuse_lighting, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(specular_lighting, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(view_depth, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(clip_depth, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(emission, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(debug_color, ETextureState::UNORDERED_ACCESS);
    DEFINE_RG_TEXTURE_ACCESS(normal, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(specular_roughness, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(motion, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(normal_roughness, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(prev_view_depth, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_PARAMETER_ACCESS(
        constants,
        ldr_color,
        diffuse_lighting,
        specular_lighting,
        view_depth,
        clip_depth,
        emission,
        debug_color,
        normal,
        specular_roughness,
        motion,
        normal_roughness,
        prev_view_depth
    );
};

} // namespace

VisualizePass::VisualizePass(RenderDevice& _device, ShaderManager& _manager) :
    device(_device),
    manager(_manager) {

    visualize_pipeline =
        _manager.Compute<VisualizePipeline>("pipelines/raytracing/passes/VisualizePass.hlsl");
    visualize_params_buffer = device.CreateBuffer<Moer::byte>(
        MOER_TEXT("Raytracing::VisualizeBuffer"), sizeof(VisualizeParams), EBufferUsageFlags::CONSTANT_BUFFER
    );
}

VisualizePass::PreparedCommand VisualizePass::Prepare(
    const RTContext&        _ctx,
    const VisualizeConfig& _cfg
) const {
    PreparedCommand command{};

    command.params.grid_params      = _ctx.is_ctx.GetGridParams();
    command.params.b_split          = _cfg.b_split;
    command.params.split_ratio      = _cfg.split_ratio;
    command.params.visualize_mode   = _cfg.visualize_mode;
    command.params.main_view        = _ctx.main_view;
    command.params.output_size      = _ctx.frame_rt.ldr_color->GetExtent().xy;

    auto div_ceil = [](uint _a, uint _b) -> uint {
        return (_a + _b - 1) / _b;
    };
    command.dispatch_groups = uint3(
        div_ceil(command.params.output_size.x, 16),
        div_ceil(command.params.output_size.y, 16),
        1
    );
    return command;
}

VisualizePass::RecordResources VisualizePass::CaptureResources(
    const RTContext& _ctx
) {
    const bool b_current_frame = _ctx.b_current_frame;
    return RecordResources{
        .ldr_color         = RTRHI(_ctx.frame_rt.ldr_color),
        .diffuse_lighting  = RTRHI(_ctx.frame_rt.diffuse_lighting),
        .specular_lighting = RTRHI(_ctx.frame_rt.specular_lighting),
        .view_depth        = RTRHI(b_current_frame ? _ctx.frame_rt.view_depth : _ctx.frame_rt.prev_view_depth),
        .clip_depth        = RTRHI(_ctx.frame_rt.clip_depth),
        .emission          = RTRHI(_ctx.frame_rt.emission),
        .normal            = RTRHI(b_current_frame ? _ctx.frame_rt.normal : _ctx.frame_rt.prev_normal),
        .specular_roughness = RTRHI(
            b_current_frame ? _ctx.frame_rt.specular_roughness : _ctx.frame_rt.prev_specular_roughness
        ),
        .motion            = RTRHI(_ctx.frame_rt.motion),
        .normal_roughness  = RTRHI(_ctx.frame_rt.normal_roughness),
        .prev_view_depth   = RTRHI(b_current_frame ? _ctx.frame_rt.prev_view_depth : _ctx.frame_rt.view_depth),
        .debug_color       = RTRHI(_ctx.frame_rt.debug_color)
    };
}

void VisualizePass::RecordConstantsUpload(CommandList& _cmdlist, const PreparedCommand& _command) {
    Array<byte> upload_data(sizeof(VisualizeParams));
    upload_data.assign((byte*)&_command.params, (byte*)&_command.params + sizeof(VisualizeParams));
    _cmdlist.CopyFrom(std::move(upload_data), visualize_params_buffer->GetView());
}

void VisualizePass::RecordVisualize(
    CommandList&           _cmdlist,
    const PreparedCommand& _command,
    const RecordResources& _resources
) {
    _cmdlist
        .Compute(
            visualize_pipeline,
            visualize_params_buffer,
            _resources.ldr_color,
            _resources.diffuse_lighting,
            _resources.specular_lighting,
            _resources.view_depth,
            _resources.clip_depth,
            _resources.emission,
            _resources.normal,
            _resources.specular_roughness,
            _resources.motion,
            _resources.normal_roughness,
            _resources.prev_view_depth,
            _resources.debug_color
        )
        .Dispatch(_command.dispatch_groups, MOER_TEXT("Visualize"));
}

void VisualizePass::AddPass(
    RenderGraph&                 _graph,
    const RTGraphFrameResources& _rg,
    const RTContext&             _ctx,
    const VisualizeConfig&       _config
) {
    auto* command   = _graph.Alloc<PreparedCommand>(Prepare(_ctx, _config));
    auto* resources = _graph.Alloc<RecordResources>(CaptureResources(_ctx));
    RGBuffer* constants = _graph.ImportBuffer(
        MOER_TEXT("RT.Visualize.constants"), visualize_params_buffer, EQueueType::Graphics
    );

    auto* upload_params      = _graph.Alloc<RGVisualizeUploadParams>();
    upload_params->constants = RGBufferView{.buffer = constants};
    _graph.AddPass(
        MOER_TEXT("RT.Visualize.UploadConstants"),
        upload_params,
        ERGPassFlags::Graphics,
        [this, command](RHICommandList& cmd_list, RGContext) {
            RecordConstantsUpload(cmd_list, *command);
        }
    );

    auto* dispatch_params                  = _graph.Alloc<RGVisualizeDispatchParams>();
    dispatch_params->constants             = RGBufferView{.buffer = constants};
    dispatch_params->ldr_color             = RTWholeTextureView(_rg.ldr_color);
    dispatch_params->diffuse_lighting      = RTWholeTextureView(_rg.diffuse_lighting);
    dispatch_params->specular_lighting     = RTWholeTextureView(_rg.specular_lighting);
    dispatch_params->view_depth            = RTWholeTextureView(_rg.current_view_depth);
    dispatch_params->clip_depth            = RTWholeTextureView(_rg.clip_depth);
    dispatch_params->emission              = RTWholeTextureView(_rg.emission);
    dispatch_params->debug_color           = RTWholeTextureView(_rg.debug_color);
    dispatch_params->normal                = RTWholeTextureView(_rg.current_normal);
    dispatch_params->specular_roughness    = RTWholeTextureView(_rg.current_specular_roughness);
    dispatch_params->motion                = RTWholeTextureView(_rg.motion);
    dispatch_params->normal_roughness      = RTWholeTextureView(_rg.normal_roughness);
    dispatch_params->prev_view_depth       = RTWholeTextureView(_rg.previous_view_depth);
    _graph.AddPass(
        MOER_TEXT("RT.Visualize.Dispatch"),
        dispatch_params,
        ERGPassFlags::Graphics | ERGPassFlags::ComputeShader,
        [this, command, resources](RHICommandList& cmd_list, RGContext) {
            RecordVisualize(cmd_list, *command, *resources);
        }
    );
}
} // namespace Moer::Render::Raytracing
