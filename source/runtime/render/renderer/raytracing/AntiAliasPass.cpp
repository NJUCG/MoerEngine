#include "AntiAliasPass.h"
#include "rhi/RHIResource.h"
#include "shader/ShaderResourceManager.h"
#include "shaderheaders/shared/postprocess/ShaderParameters.h"
#include <cmath>
#include <random>

namespace Moer::Render::Raytracing {

namespace {

struct RGTAAUploadParams {
    DEFINE_RG_BUFFER_ACCESS(constants, EBufferState::TRANSFER_DST);
    DEFINE_RG_PARAMETER_ACCESS(constants);
};

struct RGTAADispatchParams {
    DEFINE_RG_BUFFER_ACCESS(constants, EBufferState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(input, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(motion, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(feedback_read, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(output, ETextureState::UNORDERED_ACCESS);
    DEFINE_RG_TEXTURE_ACCESS(feedback_write, ETextureState::UNORDERED_ACCESS);
    DEFINE_RG_PARAMETER_ACCESS(constants, input, motion, feedback_read, output, feedback_write);
};

} // namespace

AntialiasPass::AntialiasPass(
    RenderDevice&  _device,
    ShaderManager& _manager,
    Scene&         _scene,
    CreateInfo     _info
) :
    manager(_manager),
    scene(_scene),
    taa_pipeline(manager.Compute<TAAPipeline>("pipelines/raytracing/postprocess/TAAPass.hlsl")),
    frame_idx(0),
    jitter(float2(0.f)),
    jitter_mode(EJitter::MSAA),
    motion(_info.motion),
    feedback_color_ping(_info.feedback_color_ping),
    feedback_color_pong(_info.feedback_color_pong),
    resolved_color(_info.resolved_color),
    hdr_color(_info.hdr_color) {
    constant_buffer = _device.CreateBuffer<Moer::byte>(
        MOER_TEXT("PostProcess::TAAConstantBuffer"), sizeof(TAAParams), EBufferUsageFlags::CONSTANT_BUFFER
    );
}

AntialiasPass::PreparedCommand AntialiasPass::Prepare(
    Params     _param,
    bool       _prev_view_valid,
    TextureRef _input,
    TextureRef _output
) {
    PreparedCommand command{};
    command.params.in_view_origin         = float2(0.f);
    command.params.in_view_size           = float2(_input->GetExtent().x, _input->GetExtent().y);
    command.params.out_view_origin        = float2(0.f);
    command.params.out_view_size          = float2(_output->GetExtent().x, _output->GetExtent().y);
    command.params.in_pixel_offset        = GetPixelOffset();
    command.params.out_texture_size_inv   = float2(1.f / _output->GetExtent().x, 1.f / _output->GetExtent().y);
    command.params.input_over_output_size = command.params.in_view_size / command.params.out_view_size;
    command.params.output_over_input_size = command.params.out_view_size / command.params.in_view_size;
    command.params.clamping_factor        = _param.enable_history_clamp ? _param.clamping_factor : -1.f;
    command.params.new_frame_weight       = _prev_view_valid ? _param.new_frame_weight : 1.f;
    command.params.pqc                    = std::clamp(_param.max_radiance, 1e-4f, 1e8f);
    command.params.inv_pqc                = 1.f / command.params.pqc;
    command.dispatch_groups = uint3((_output->GetExtent().x + 15) / 16, ((_output->GetExtent().y + 15) / 16), 1);
    return command;
}

AntialiasPass::RecordResources AntialiasPass::CaptureResources(TextureRef _input, TextureRef _output) const {
    return RecordResources{
        .input          = _input,
        .output         = _output,
        .motion         = motion,
        .feedback_read  = feedback_color_ping,
        .feedback_write = feedback_color_pong
    };
}

void AntialiasPass::RecordConstantsUpload(CommandList& _cmd_list, const PreparedCommand& _command) {
    Array<Moer::byte> upload_data(sizeof(TAAParams));
    upload_data.assign((Moer::byte*)&_command.params, (Moer::byte*)&_command.params + sizeof(TAAParams));

    _cmd_list.CopyFrom(std::move(upload_data), constant_buffer->GetView());
}

void AntialiasPass::RecordTAA(
    CommandList&           _cmd_list,
    const PreparedCommand& _command,
    const RecordResources& _resources
) {
    Sampler linear_sampler{ESamplerFilter::SF_LINEAR, ESamplerAddressMode::SAM_CLAMP_TO_EDGE};
    _cmd_list
        .Compute(
            taa_pipeline,
            constant_buffer,
            _resources.input,
            _resources.motion,
            _resources.feedback_read,
            linear_sampler,
            _resources.output,
            _resources.feedback_write
        )
        .Dispatch(_command.dispatch_groups, MOER_TEXT("TAAPass"));
}

void AntialiasPass::AddPass(
    RenderGraph&                 _graph,
    const RTGraphFrameResources& _rg,
    const RTContext&             _rt_ctx,
    Params                       _params,
    bool                         _prev_view_valid,
    TextureRef                   _input,
    TextureRef                   _output
) {
    auto* command = _graph.Alloc<PreparedCommand>(Prepare(_params, _prev_view_valid, _input, _output));
    auto* resources = _graph.Alloc<RecordResources>(CaptureResources(_input, _output));
    RGBuffer* constants = _graph.ImportBuffer(MOER_TEXT("RT.TAA.constants"), constant_buffer, EQueueType::Graphics);
    RGTexture* input_rg       = RTGraphTextureForFrameTexture(_rg, _rt_ctx, _input);
    RGTexture* output_rg      = RTGraphTextureForFrameTexture(_rg, _rt_ctx, _output);
    RGTexture* feedback_read  = ImportedRTFeedbackTexture(_rg, _rt_ctx, FeedbackReadTexture());
    RGTexture* feedback_write = ImportedRTFeedbackTexture(_rg, _rt_ctx, FeedbackWriteTexture());

    auto* upload_params      = _graph.Alloc<RGTAAUploadParams>();
    upload_params->constants = RGBufferView{.buffer = constants};
    _graph.AddPass(
        MOER_TEXT("RT.AntiAlias.UploadConstants"),
        upload_params,
        ERGPassFlags::Graphics,
        [this, command](RHICommandList& cmd_list, RGContext) {
            RecordConstantsUpload(cmd_list, *command);
        }
    );

    auto* dispatch_params           = _graph.Alloc<RGTAADispatchParams>();
    dispatch_params->constants      = RGBufferView{.buffer = constants};
    dispatch_params->input          = RTWholeTextureView(input_rg);
    dispatch_params->motion         = RTWholeTextureView(_rg.motion);
    dispatch_params->feedback_read  = RTWholeTextureView(feedback_read);
    dispatch_params->output         = RTWholeTextureView(output_rg);
    dispatch_params->feedback_write = RTWholeTextureView(feedback_write);
    _graph.AddPass(
        MOER_TEXT("RT.AntiAlias.Dispatch"),
        dispatch_params,
        kRTGraphComputePassFlags,
        [this, command, resources](RHICommandList& cmd_list, RGContext) {
            RecordTAA(cmd_list, *command, *resources);
        }
    );
}

void AntialiasPass::AdvanceFrame() {
    frame_idx++;

    if (jitter_mode == EJitter::R2) {
        static constexpr float g  = 1.32471795724474602596f;
        static constexpr float a1 = 1.f / g;
        static constexpr float a2 = 1.f / (g * g);
        jitter[0]                 = std::fmodf(jitter[0] + a1, 1.f);
        jitter[1]                 = std::fmodf(jitter[1] + a2, 1.f);
    }

    std::swap(feedback_color_ping, feedback_color_pong);
}

const TextureRef& AntialiasPass::FeedbackReadTexture() const {
    return feedback_color_ping;
}

const TextureRef& AntialiasPass::FeedbackWriteTexture() const {
    return feedback_color_pong;
}

static float VanderCorput(uint _idx, uint _base) {
    float v = 0.f;
    float f = 1.f / _base;
    uint  i = _idx;
    while (i > 0) {
        v += (i % _base) * f;
        i /= _base;
        f /= _base;
    }
    return v;
}

float2 AntialiasPass::GetPixelOffset() {
    switch (jitter_mode) {

        case EJitter::MSAA: {
            const float2 offsets[] = {
                float2(0.0625f, -0.1875f),
                float2(-0.0625f, 0.1875f),
                float2(0.3125f, 0.0625f),
                float2(-0.1875f, -0.3125f),
                float2(-0.3125f, 0.3125f),
                float2(-0.4375f, 0.0625f),
                float2(0.1875f, 0.4375f),
                float2(0.4375f, -0.4375f)
            };
            return offsets[frame_idx % 8];
        }
        case EJitter::Halton: {
            uint idx = (frame_idx % 16) + 1;
            return float2(VanderCorput(idx, 2), VanderCorput(idx, 3)) - 0.5f;
        }
        case EJitter::R2: {
            return jitter - 0.5f;
        }
        case EJitter::WhiteNoise: {
            std::mt19937_64                       rng(frame_idx);
            std::uniform_real_distribution<float> dist(-0.5f, 0.5f);
            return float2(dist(rng), dist(rng));
        }
        case EJitter::Num:
            break;
    }
    return float2(0.f);
}

void AntialiasPass::SetJitter(EJitter _jitter_mode) {
    jitter_mode = _jitter_mode;
}

} // namespace Moer::Render::Raytracing