#ifndef MOER_ANTIALIASPASS_H
#define MOER_ANTIALIASPASS_H

// 提供时序抗锯齿 Pass 和逐帧相机抖动。

#include "RaytracingGraphResources.h"
#include "RaytracingConfig.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHIResource.h"
#include "shaderheaders/shared/postprocess/ShaderParameters.h"

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
    };

    struct PreparedCommand {
        TAAParams params{};
        uint3     dispatch_groups{};
    };

    struct RecordResources {
        TextureRef input{};
        TextureRef output{};
        TextureRef motion{};
        TextureRef feedback_read{};
        TextureRef feedback_write{};
    };

    AntialiasPass(RenderDevice& device, class ShaderManager& manager, CreateInfo info);

    void Process(
        CommandList& _cmd_list,
        Params       _params,
        bool         _prev_view_valid,
        TextureRef   _input,
        TextureRef   _output
    );
    bool AddPasses(
        RenderGraph&                 graph,
        const RTGraphFrameResources& graph_resources,
        const RTContext&             rt_ctx,
        Params                       params,
        bool                         prev_view_valid,
        TextureRef                   input,
        TextureRef                   output
    );
    void   CommitAcceptedFrame();
    void   SetJitter(EJitter _jitter_mode);
    float2 GetPixelOffset() const;
    bool   IsHistoryReadyForGraph() const;

private:
    struct RecordingOwner {
        BufferRef   constants{};
        TAAPipeline pipeline{};
    };

    PreparedCommand Prepare(
        Params            params,
        bool              prev_view_valid,
        const TextureRef& input,
        const TextureRef& output
    ) const;
    RecordResources CaptureResources(
        TextureRef input,
        TextureRef output
    ) const;
    static void RecordConstantsUpload(
        CommandList&           cmd_list,
        const RecordingOwner&  owner,
        const PreparedCommand& command
    );
    static void RecordTAA(
        CommandList&           cmd_list,
        RecordingOwner&        owner,
        const PreparedCommand& command,
        const RecordResources& resources
    );

    uint    frame_idx   = 0;
    float2  jitter      = float2(0.f);
    EJitter jitter_mode = EJitter::MSAA;

    TextureRef motion;
    TextureRef feedback_color_ping;
    TextureRef feedback_color_pong;
    TextureRef feedback_slot_zero;
    TextureRef feedback_slot_one;
    uint8      initialized_history_mask = 0;

    SharedPtr<RecordingOwner> recording_owner;
};
} // namespace Moer::Render::Raytracing
#endif
