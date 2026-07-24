#ifndef MOER_RENDER_GBUFFER_PASS_H
#define MOER_RENDER_GBUFFER_PASS_H

// 生成 Raytracing GBuffer，并转换为降噪器所需布局。

#include "RaytracingGraphResources.h"
#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/ShaderParameters.h"

namespace Moer::Render::Raytracing {

class RaytracingGBufferPipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(RaytracingGBufferPipeline);

    DEFINE_SHADER_CONSTANT_STRUCT(GBufferPassParams, param);
    DEFINE_SHADER_BUFFER(gbuffer_constants);
    DEFINE_SHADER_TEX(gbuffer_view_depth);
    DEFINE_SHADER_TEX(gbuffer_diffuse_albedo);
    DEFINE_SHADER_TEX(gbuffer_specular_roughness);
    DEFINE_SHADER_TEX(gbuffer_normal);
    DEFINE_SHADER_TEX(gbuffer_emissive);
    DEFINE_SHADER_TEX(gbuffer_motion);
    DEFINE_SHADER_TEX(gbuffer_clip_depth);
    DEFINE_SHADER_TLAS(tlas);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);

    DEFINE_SHADER_ARGS(
        param,
        gbuffer_constants,
        gbuffer_view_depth,
        gbuffer_diffuse_albedo,
        gbuffer_specular_roughness,
        gbuffer_normal,
        gbuffer_emissive,
        gbuffer_motion,
        gbuffer_clip_depth,
        tlas,
        bdls
    );

    MUTATION_BOOL(PRINT_TEST);
};

MUTATION_SET(RTGBufferMacros, RaytracingGBufferPipeline::PRINT_TEST);

class PostProcessGBufferPipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(PostProcessGBufferPipeline);

    DEFINE_SHADER_TEX(rw_specular_roughness);
    DEFINE_SHADER_TEX(rw_normal_roughness);
    DEFINE_SHADER_TEX(r_normal);
    DEFINE_SHADER_TEX(r_view_depth);

    DEFINE_SHADER_ARGS(rw_specular_roughness, rw_normal_roughness, r_normal, r_view_depth);

    // WITH_NRD 同时也是构建宏，因此暂时释放该名称供 Shader 变体使用。
#pragma push_macro("WITH_NRD")
#undef WITH_NRD
    MUTATION_SPARSE_UINT(WITH_NRD, 0, 1);
    MUTATION_SET(MutationSet, WITH_NRD);
#pragma pop_macro("WITH_NRD")
};

class GBufferPass {
public:
    struct PreparedCommand {
        GBufferPassParams params{};
        GBufferConstants  constants{};
        uint3             dispatch_groups{};
    };

    struct RecordResources {
        TextureRef        view_depth{};
        TextureRef        diffuse_albedo{};
        TextureRef        specular_roughness{};
        TextureRef        normal{};
        TextureRef        emission{};
        TextureRef        motion{};
        TextureRef        clip_depth{};
        TextureRef        normal_roughness{};
        RaytracingTlasRef tlas{};
        BindlessArrayRef  bindless_array{};
    };

    GBufferPass(class RenderDevice& device, class ShaderManager& manager, BindlessArrayRef bindless_array);
    void Process(class CommandList& cmd_list, RTContext& rt_ctx);
    void RecordLegacyTailBridge(class CommandList& cmd_list, const RTContext& rt_ctx) const;
    bool AddPasses(
        RenderGraph&                 graph,
        const RTGraphFrameResources& graph_resources,
        const RTContext&             rt_ctx,
        bool                         tlas_built_this_frame,
        bool                         normal_roughness_readable
    );

private:
    PreparedCommand Prepare(const RTContext& rt_ctx) const;
    RecordResources CaptureResources(const RTContext& rt_ctx) const;
    void RecordConstantsUpload(CommandList& cmd_list, const PreparedCommand& command);
    void RecordTraceGBuffer(
        CommandList&           cmd_list,
        const PreparedCommand& command,
        const RecordResources& resources
    );
    void RecordPostProcessGBuffer(
        CommandList&           cmd_list,
        const PreparedCommand& command,
        const RecordResources& resources
    );

    BindlessArrayRef bindless_array;

    BufferRef gbuffer_constants;

    RaytracingGBufferPipeline  gbuffer_pipeline;
    PostProcessGBufferPipeline gbuffer_postprocess_pipeline;
};

} // namespace Moer::Render::Raytracing

#endif
