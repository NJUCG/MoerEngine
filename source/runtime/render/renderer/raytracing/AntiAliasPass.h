#ifndef MOER_ANTIALIASPASS_H
#define MOER_ANTIALIASPASS_H

#include "RTResource.h"
#include "RaytracingConfig.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHIResource.h"


namespace Moer::Render::Raytracing {

class TAAPipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(TAAPipeline);

    DEFINE_SHADER_BUFFER(params);
    DEFINE_SHADER_TEX(unfiltered_rt);
    DEFINE_SHADER_TEX(motion);
    DEFINE_SHADER_TEX(feedback_rt);
    DEFINE_SHADER_SAMPLER(s_sampler);
    DEFINE_SHADER_TEX(output_rt);
    DEFINE_SHADER_TEX(rw_feedback_rt);

    DEFINE_SHADER_ARGS(params, unfiltered_rt, motion, feedback_rt, s_sampler, output_rt, rw_feedback_rt);
};
class AntialiasPass {
public:
    struct Params {
        float new_frame_weight     = 0.04f;
        float clamping_factor      = 1.3f;
        float max_radiance         = 200.f;
        float enable_history_clamp = true;
    };

    struct CreateInfo {
        TextureRef motion;
        TextureRef feedback_color_ping;
        TextureRef feedback_color_pong;
        TextureRef resolved_color;
        TextureRef hdr_color;
    };

    AntialiasPass(RenderDevice& _device, class ShaderManager& _manager, Scene& _scene, CreateInfo _info);

    void Process(
        CommandList& _cmd_list,
        RTContext&   _rt_ctx,
        Params       _params,
        bool         _prev_view_valid,
        TextureRef   _input,
        TextureRef   _output
    );
    void   AdvanceFrame();
    void   SetJitter(EJitter _jitter_mode);
    float2 GetPixelOffset();

private:
private:
    ShaderManager& manager;
    Scene&         scene;
    TAAPipeline    taa_pipeline;

    uint    frame_idx   = 0;
    float2  jitter      = float2(0.f);
    EJitter jitter_mode = EJitter::MSAA;

    BufferRef constant_buffer;

    TextureRef motion;
    TextureRef feedback_color_ping;
    TextureRef feedback_color_pong;
    TextureRef resolved_color;
    TextureRef hdr_color;
};
} // namespace Moer::Render::Raytracing
#endif