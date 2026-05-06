#pragma once

#include "math/Function.h"
#include "scene/camera/Camera.h"
#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/raster/post_process/ShaderParameters.h"
#include "utils/smaa/SmaaPrecomputedTextures.h"

#include "RasterConfig.h"
#include "RasterResource.h"
#include "RasterTool.h"

namespace Moer::Render::Raster {

class SmaaEdgeDetectionPipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(SmaaEdgeDetectionPipeline);
    DEFINE_SHADER_CONSTANT_STRUCT(SmaaSharedPipelineBindlessParam, param);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_ARGS(bdls, param);
};
class SmaaBlendingWeightPipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(SmaaBlendingWeightPipeline);
    DEFINE_SHADER_CONSTANT_STRUCT(SmaaSharedPipelineBindlessParam, param);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_ARGS(bdls, param);
};
class SmaaNeighborhoodBlendingPipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(SmaaNeighborhoodBlendingPipeline);
    DEFINE_SHADER_CONSTANT_STRUCT(SmaaSharedPipelineBindlessParam, param);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_ARGS(bdls, param);
};
class SmaaT2xNeighborhoodBlendingPipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(SmaaT2xNeighborhoodBlendingPipeline);
    DEFINE_SHADER_CONSTANT_STRUCT(SmaaSharedPipelineBindlessParam, param);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_ARGS(bdls, param);
};
class SmaaT2xResolvePipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(SmaaT2xResolvePipeline);
    DEFINE_SHADER_CONSTANT_STRUCT(SmaaSharedPipelineBindlessParam, param);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_ARGS(bdls, param);
};

class FxaaPrecomputePipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(FxaaPrecomputePipeline);
    DEFINE_SHADER_CONSTANT_STRUCT(FxaaPrecomputePipelineBindlessParam, param);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_ARGS(bdls, param);
};

class FxaaPipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(FxaaPipeline);
    DEFINE_SHADER_CONSTANT_STRUCT(FxaaPipelineBindlessParam, param);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_ARGS(bdls, param);
};

/**
 * MARK: AA Passes
 * 
 * Use gui to switch antialiasing mode:
 * 0: FXAA Off                : 620+-fps
 * 1: FXAA Quality(Simplified): 612+-fps
 * 2: FXAA Quality            : 584+-fps
 * 3: SMAA 1x  (Preset High)  : 578+-fps [Default]
 * 4: SMAA T2x (Preset High)  : 
 * 
 * For FXAA (2 passes)
 *   Pass 1: precompute luma -> antialiasing_temporal_texture_1
 *   Pass 2: FXAA main pass  -> antiailiasing_output
 * 
 * For SMAA 1x (3 passes, details in shaders/test/post_process/SMAA.hlsl)
 *   Pass 1: Edge Detection              -> antialiasing_temporal_texture_1 (edgesTex)
 *   Pass 2: Blending Weight Calculation -> antialiasing_temporal_texture_2 (blendTex)
 *   Pass 3: Neighborhood Blending       -> antialiasing_output
 * 
 * For SMAA T2x (4 passes)
 *   Pass 1: Edge Detection              -> antialiasing_temporal_texture_1  (rg: edgesTex & ba: velocityTex)
 *   Pass 2: Blending Weight Calculation -> antialiasing_temporal_texture_2  (blendTex)
 *   Pass 3: Neighborhood Blending       -> antialiasing_temporal_texture_34 (double buffer) (currentColorTex & previousColorTex)
 *   Pass 4: Resolve                     -> antialiasing_output
 *   Note: SMAA T2x needs reprojection to prevent ghosting. Reprojection needs velocityTex,
 *   which is currently packed into the later channels of edgesTex.
 *   Note 2: Ghosting is hard to observe at 500+ fps; adding a 0.1s loop sleep and setting
 *   SMAAReprojection to 0 in the shader can produce a visible ghosting reference.
 *
 * Mode switching note:
 *   Several passes may be skipped when the AA mode changes, so stale resources must not be assumed valid.
 *
 * SMAA implementation notes:
 *   The implementation is adapted from https://github.com/iryoku/smaa.
 *   The original code does not use bindless RHI, so texture and sampler fetching is wrapped for this engine.
 *   The shader side is tightly coupled to the bindless RHI layout; bindless layout changes require matching shader updates.
 *   If bindless changes break this path, switch temporarily to FXAA (aa_mode = 2) to unblock rendering.
 *
 * SMAA T2x notes:
 *   1. T2x uses temporal supersampling and needs camera jitter through camera->SetJitteredMatrix().
 *   2. Current SMAA T2x quality is close to SMAA 1x; this may be a shader or integration issue.
 *   FIXME: fix jitter in SMAA T2x when SMAA_REPROJECTION is enabled
 */
class AaPass {
public:
    AaPass(RasterContext& context) {

        auto& tex    = context.textures;
        auto  format = tex.aa_output.tex->GetFormat();

        assert(format == tex.aa_texture_1.tex->GetFormat());
        assert(format == tex.aa_texture_2.tex->GetFormat());
        assert(format == tex.aa_texture_3.tex->GetFormat());

        // smaa
        smaa_edge_detection_pipeline = [&]() {
            GfxPsoCreateInfo pso_full_screen_info(
                RHIRasterizeInfo::Preset(), {}, {RHIColorAttachmentInfo::Preset(format)}
            );
            return context.manager.Raster()
                .Vertex("pipelines/postprocess/aa/SmaaWrapper.hlsl", "SMAAEdgeDetectionVS_Wrapper")
                .Pixel("pipelines/postprocess/aa/SmaaWrapper.hlsl", "SMAALumaEdgeDetectionPS_Wrapper")
                .Build<SmaaEdgeDetectionPipeline>(std::move(pso_full_screen_info));
        }();

        smaa_blending_weight_pipeline = [&]() {
            GfxPsoCreateInfo pso_full_screen_info(
                RHIRasterizeInfo::Preset(), {}, {RHIColorAttachmentInfo::Preset(format)}
            );
            return context.manager.Raster()
                .Vertex(
                    "pipelines/postprocess/aa/SmaaWrapper.hlsl", "SMAABlendingWeightCalculationVS_Wrapper"
                )
                .Pixel("pipelines/postprocess/aa/SmaaWrapper.hlsl", "SMAABlendingWeightCalculationPS_Wrapper")
                .Build<SmaaBlendingWeightPipeline>(std::move(pso_full_screen_info));
        }();

        smaa_neighborhood_blending_pipeline = [&]() {
            GfxPsoCreateInfo pso_full_screen_info(
                RHIRasterizeInfo::Preset(), {}, {RHIColorAttachmentInfo::Preset(format)}
            );
            return context.manager.Raster()
                .Vertex("pipelines/postprocess/aa/SmaaWrapper.hlsl", "SMAANeighborhoodBlendingVS_Wrapper")
                .Pixel("pipelines/postprocess/aa/SmaaWrapper.hlsl", "SMAANeighborhoodBlendingPS_Wrapper")
                .Build<SmaaNeighborhoodBlendingPipeline>(std::move(pso_full_screen_info));
        }();

        smaa_t2x_neighborhood_blending_pipeline = [&]() {
            GfxPsoCreateInfo pso_full_screen_info(
                RHIRasterizeInfo::Preset(), {}, {RHIColorAttachmentInfo::Preset(format)}
            );
            return context.manager.Raster()
                .Vertex("pipelines/postprocess/aa/SmaaWrapper.hlsl", "SMAANeighborhoodBlendingVS_Wrapper")
                .Pixel("pipelines/postprocess/aa/SmaaWrapper.hlsl", "SMAANeighborhoodBlendingPS_Wrapper")
                .Build<SmaaT2xNeighborhoodBlendingPipeline>(std::move(pso_full_screen_info));
        }();

        smaa_t2x_resolve_pipeline = [&]() {
            GfxPsoCreateInfo pso_full_screen_info(
                RHIRasterizeInfo::Preset(), {}, {RHIColorAttachmentInfo::Preset(format)}
            );
            return context.manager.Raster()
                .Vertex("pipelines/postprocess/aa/SmaaWrapper.hlsl", "SMAAResolveVS_Wrapper")
                .Pixel("pipelines/postprocess/aa/SmaaWrapper.hlsl", "SMAAResolvePS_Wrapper")
                .Build<SmaaT2xResolvePipeline>(std::move(pso_full_screen_info));
        }();

        // fxaa
        fxaa_precompute_pipeline = [&]() {
            GfxPsoCreateInfo pso_full_screen_info(
                RHIRasterizeInfo::Preset(), {}, {RHIColorAttachmentInfo::Preset(format)}
            );
            return context.manager.Raster()
                .Vertex("core/utils/FullScreenQuad.hlsl")
                .Pixel("pipelines/postprocess/aa/FxaaPrecompute.hlsl")
                .Build<FxaaPrecomputePipeline>(std::move(pso_full_screen_info));
        }();

        fxaa_pipeline = [&]() {
            GfxPsoCreateInfo pso_full_screen_info(
                RHIRasterizeInfo::Preset(), {}, {RHIColorAttachmentInfo::Preset(format)}
            );
            return context.manager.Raster()
                .Vertex("core/utils/FullScreenQuad.hlsl")
                .Pixel("pipelines/postprocess/aa/Fxaa.hlsl")
                .Build<FxaaPipeline>(std::move(pso_full_screen_info));
        }();

        // Other resources initialization
        aa_texture_34[0] = &tex.aa_texture_3;
        aa_texture_34[1] = &tex.aa_texture_4;
        frame_parity     = 0;

        LoadSmaaResources(context);
    }

    void LoadSmaaResources(RasterContext& context) {
        Sampler linear_sampler(SF_LINEAR, SAM_REPEAT);

        // smaa area tex
        smaa_area_tex.tex = context.device.CreateTexture(
            MOER_TEXT("smaa_area_tex"),
            Extent2D(SMAA_AREATEX_WIDTH, SMAA_AREATEX_HEIGHT),
            PF_R8G8_UNORM,
            ETextureUsageFlags::SAMPLED | ETextureUsageFlags::TRANSFER_DST
        );
        context.cmd_list.CopyFrom(
            std::span<byte>(
                (byte*)&SmaaPrecomputedTextures::areaTexBytes, sizeof(SmaaPrecomputedTextures::areaTexBytes)
            ),
            smaa_area_tex.tex
        );
        smaa_area_tex.hdl = context.bdls->AllocateTexture(smaa_area_tex.tex, linear_sampler);

        // smaa search tex
        smaa_search_tex.tex = context.device.CreateTexture(
            MOER_TEXT("smaa_search_tex"),
            Extent2D(SMAA_SEARCHTEX_WIDTH, SMAA_SEARCHTEX_HEIGHT),
            PF_R8_UNORM,
            ETextureUsageFlags::SAMPLED | ETextureUsageFlags::TRANSFER_DST
        );
        context.cmd_list.CopyFrom(
            std::span<byte>(
                (byte*)&SmaaPrecomputedTextures::searchTexBytes,
                sizeof(SmaaPrecomputedTextures::searchTexBytes)
            ),
            smaa_search_tex.tex
        );
        smaa_search_tex.hdl = context.bdls->AllocateTexture(smaa_search_tex.tex, linear_sampler);
    }

    TextureWithHandle Process(
        RasterContext&      context,
        const RasterConfig& ui_config,
        const Camera&       camera,
        TextureWithHandle   input_image
    ) {
        if (ui_config.aa_mode == EAaMode::NONE || ui_config.aa_mode == EAaMode::FXAA_SIMPLIFIED ||
            ui_config.aa_mode == EAaMode::FXAA_QUALITY) {
            return ProcessFxaa(context, ui_config, camera, input_image);
        }

        if (ui_config.aa_mode == EAaMode::SMAA_1X || ui_config.aa_mode == EAaMode::SMAA_T2X) {
            return ProcessSmaa(context, ui_config, camera, input_image);
        }

        assert(false && "Invalid antialiasing mode");
        return input_image;
    }

    TextureWithHandle ProcessFxaa(
        RasterContext&      context,
        const RasterConfig& ui_config,
        const Camera&       camera,
        TextureWithHandle   input_image
    ) {
        FxaaPrecomputePipelineBindlessParam param_fxaa_precomputed;
        param_fxaa_precomputed.input_image = input_image.hdl;

        context.cmd_list.Gfx(fxaa_precompute_pipeline, context.bdls, param_fxaa_precomputed)
            .Draw(MOER_TEXT("FXAA Precompute Pass"),
                context.textures.aa_texture_1.GetRect2D(),
                std::move(RasterTool::GetFullScreenDrawDatas()),
                ColorAttachment(context.textures.aa_texture_1.tex)
            );

        FxaaPipelineBindlessParam param_fxaa;
        param_fxaa.input_image    = context.textures.aa_texture_1.hdl;
        param_fxaa.fxaa_mode      = static_cast<uint32>(ui_config.aa_mode);
        param_fxaa.resolution     = float2(context.textures.aa_output.GetSize());
        param_fxaa.inv_resolution = float2(1.0) / float2(context.textures.aa_output.GetSize());

        context.cmd_list.Gfx(fxaa_pipeline, context.bdls, param_fxaa)
            .Draw(MOER_TEXT("FXAA Pass"),
                context.textures.aa_output.GetRect2D(),
                std::move(RasterTool::GetFullScreenDrawDatas()),
                ColorAttachment(context.textures.aa_output.tex)
            );

        return context.textures.aa_output;
    }

    TextureWithHandle ProcessSmaa(
        RasterContext&      context,
        const RasterConfig& ui_config,
        const Camera&       camera,
        TextureWithHandle   input_image
    ) {
        // TODO: simplify sampler index extraction when bindless sampler metadata is exposed directly.
        // SMAA.hlsl was adapted from the official implementation and now fetches textures and samplers
        // through bindless RHI. Texture indices come from AllocateTexture; sampler indices are currently
        // reconstructed locally to match VulkanDevice::GetSamplerIdx().
        auto GetSamplerIdx = [&](const Sampler& sampler) {
            // method 1
            uint filter  = uint(sampler.filter);
            uint address = uint(sampler.address_mode);
            uint compare = uint(sampler.compare_function);
            return (uint(SF_Num) * uint(SAM_Num)) * compare + (uint(SF_Num)) * address + filter;
            // method 2
            // uint bdls_tex_handle = context.bdls->AllocateTexture(antialiasing_output, sampler);
            // uint sampler_idx     = bdls_tex_handle & 0xff;
            // return sampler_idx;
        };

        static Matrix4x4f current_view_proj     = Matrix4x4f::Identity();
        static Matrix4x4f previous_view_proj    = Matrix4x4f::Identity();
        static Matrix4x4f current_inv_view_proj = Matrix4x4f::Identity();

        previous_view_proj    = current_view_proj;
        current_view_proj     = camera.GetViewProjectionMatrix();
        current_inv_view_proj = camera.GetViewProjectionMatrixInv();

        frame_parity ^= 1;

        auto smaa_shared_param = [&]() {
            SmaaSharedPipelineBindlessParam param;
            param.clip2world         = Transpose(camera.GetViewProjectionMatrixInv());
            param.aa_mode            = static_cast<uint32>(ui_config.aa_mode);
            param.color_tex          = input_image.hdl;
            param.depth_tex          = context.textures.depth_linear_sampler.hdl;
            param.search_tex         = smaa_search_tex.hdl;
            param.area_tex           = smaa_area_tex.hdl;
            param.edges_tex          = context.textures.aa_texture_1.hdl;
            param.blend_tex          = context.textures.aa_texture_2.hdl;
            param.current_color_tex  = aa_texture_34[frame_parity]->hdl;
            param.previous_color_tex = aa_texture_34[frame_parity ^ 1]->hdl;
            param.frame_index        = frame_parity;
            param.point_sampler      = GetSamplerIdx(Sampler(SF_NEAREST, SAM_CLAMP_TO_EDGE));
            param.linear_sampler     = GetSamplerIdx(Sampler(SF_LINEAR, SAM_CLAMP_TO_EDGE));
            param.rt_metrics         = float4(
                1.0f / context.textures.aa_output.GetSizeX(),
                1.0f / context.textures.aa_output.GetSizeY(),
                context.textures.aa_output.GetSizeX(),
                context.textures.aa_output.GetSizeY()
            );
            param.clip2prev_clip = Transpose(previous_view_proj * current_inv_view_proj);
            return param;
        }();

        context.cmd_list.Gfx(smaa_edge_detection_pipeline, context.bdls, smaa_shared_param)
            .Draw(MOER_TEXT("SMAA Edge Detection Pass"),
                context.textures.aa_texture_1.GetRect2D(),
                std::move(RasterTool::GetFullScreenDrawDatas()),
                ColorAttachment(context.textures.aa_texture_1.tex)
            );

        context.cmd_list.Gfx(smaa_blending_weight_pipeline, context.bdls, smaa_shared_param)
            .Draw(MOER_TEXT("SMAA Blending Weight Calculation Pass"),
                context.textures.aa_texture_2.GetRect2D(),
                std::move(RasterTool::GetFullScreenDrawDatas()),
                ColorAttachment(context.textures.aa_texture_2.tex)
            );

        if (ui_config.aa_mode == EAaMode::SMAA_1X) {
            context.cmd_list.Gfx(smaa_neighborhood_blending_pipeline, context.bdls, smaa_shared_param)
                .Draw(MOER_TEXT("SMAA Neighborhood Blending Pass"),
                    context.textures.aa_output.GetRect2D(),
                    std::move(RasterTool::GetFullScreenDrawDatas()),
                    ColorAttachment(context.textures.aa_output.tex)
                );
        } else if (ui_config.aa_mode == EAaMode::SMAA_T2X) {
            context.cmd_list.Gfx(smaa_t2x_neighborhood_blending_pipeline, context.bdls, smaa_shared_param)
                .Draw(MOER_TEXT("SMAA T2x Neighborhood Blending Pass"),
                    context.textures.aa_texture_3.GetRect2D(),
                    std::move(RasterTool::GetFullScreenDrawDatas()),
                    ColorAttachment(aa_texture_34[frame_parity]->tex)
                );

            context.cmd_list.Gfx(smaa_t2x_resolve_pipeline, context.bdls, smaa_shared_param)
                .Draw(MOER_TEXT("SMAA T2x Resolve Pass"),
                    context.textures.aa_output.GetRect2D(),
                    std::move(RasterTool::GetFullScreenDrawDatas()),
                    ColorAttachment(context.textures.aa_output.tex)
                );
        } else {
            assert(false && "Invalid antialiasing mode");
        }

        return context.textures.aa_output;
    }

private:
    // smaa
    SmaaEdgeDetectionPipeline           smaa_edge_detection_pipeline;
    SmaaBlendingWeightPipeline          smaa_blending_weight_pipeline;
    SmaaNeighborhoodBlendingPipeline    smaa_neighborhood_blending_pipeline;
    SmaaT2xNeighborhoodBlendingPipeline smaa_t2x_neighborhood_blending_pipeline;
    SmaaT2xResolvePipeline              smaa_t2x_resolve_pipeline;

    // smaa resources
    StaticArray<TextureWithHandle*, 2> aa_texture_34;
    uint8                              frame_parity = 0;

    TextureWithHandle smaa_area_tex;
    TextureWithHandle smaa_search_tex;

    // fxaa
    FxaaPrecomputePipeline fxaa_precompute_pipeline;
    FxaaPipeline           fxaa_pipeline;
};

} // namespace Moer::Render::Raster