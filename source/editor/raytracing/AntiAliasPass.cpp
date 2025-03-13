#include "AntiAliasPass.h"
#include "rhi/RHIResource.h"
#include "shader/ShaderResourceManager.h"
#include "shaderheaders/shared/postprocess/ShaderParameters.h"
#include <cmath>
#include <random>

namespace Moer::Render::Raytracing {

AntialiasPass::AntialiasPass(
    RenderDevice&  _device,
    ShaderManager& _manager,
    Scene&         _scene,
    CreateInfo     _info
) :
    manager(_manager),
    scene(_scene),
    taa_pipeline(manager.Compute<TAAPipeline>("postprocess/TAAPass.hlsl")),
    frame_idx(0),
    jitter(float2(0.f)),
    jitter_mode(EJitter::MSAA),
    motion(_info.motion),
    feedback_color_ping(_info.feedback_color_ping),
    feedback_color_pong(_info.feedback_color_pong),
    resolved_color(_info.resolved_color),
    hdr_color(_info.hdr_color) {
    constant_buffer = _device.CreateBuffer<Moer::byte>(sizeof(TAAParams), EBufferUsageFlags::CONSTANT_BUFFER);
}

void AntialiasPass::Process(
    CommandList& _cmd_list,
    RTContext&   _rt_ctx,
    Params       _param,
    bool         _prev_view_valid,
    TextureRef   _input,
    TextureRef   _output
) {
    TAAParams params{};
    params.in_view_origin         = float2(0.f);
    params.in_view_size           = float2(_input->GetExtent().x, _input->GetExtent().y);
    params.out_view_origin        = float2(0.f);
    params.out_view_size          = float2(_output->GetExtent().x, _output->GetExtent().y);
    params.in_pixel_offset        = GetPixelOffset();
    params.out_texture_size_inv   = float2(1.f / _output->GetExtent().x, 1.f / _output->GetExtent().y);
    params.input_over_output_size = params.in_view_size / params.out_view_size;
    params.output_over_input_size = params.out_view_size / params.in_view_size;
    params.clamping_factor        = _param.enable_history_clamp ? _param.clamping_factor : -1.f;
    params.new_frame_weight       = _prev_view_valid ? _param.new_frame_weight : 1.f;
    params.pqc                    = std::clamp(_param.max_radiance, 1e-4f, 1e8f);
    params.inv_pqc                = 1.f / params.pqc;

    _cmd_list.PushScope("AntiAliasPass");
    Array<Moer::byte> upload_data(sizeof(TAAParams));
    upload_data.assign((Moer::byte*)&params, (Moer::byte*)&params + sizeof(TAAParams));

    _cmd_list.CopyFrom(std::move(upload_data), constant_buffer->GetView());

    uint2 grid_size = uint2((_output->GetExtent().x + 15) / 16, ((_output->GetExtent().y + 15) / 16));

    Sampler linear_sampler{ESamplerFilter::SF_LINEAR, ESamplerAddressMode::SAM_CLAMP_TO_EDGE};
    _cmd_list
        .Compute(
            taa_pipeline,
            constant_buffer,
            _input,
            motion,
            feedback_color_ping,
            linear_sampler,
            _output,
            feedback_color_pong
        )
        .Dispatch(uint3(grid_size.x, grid_size.y, 1), "TAAPass");

    _cmd_list.PopScope();
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
        case EJitter::Num: break;
    }
    return float2(0.f);
}

void AntialiasPass::SetJitter(EJitter _jitter_mode) { jitter_mode = _jitter_mode; }

} // namespace Moer::Render::Raytracing