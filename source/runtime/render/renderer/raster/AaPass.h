#pragma once

#include "math/Function.h"
#include "misc/Types.h"
#include "rendergraph/RenderGraphUpload.h"
#include "scene/camera/Camera.h"
#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/raster/post_process/ShaderParameters.h"
#include "utils/smaa/SmaaPrecomputedTextures.h"

#include "RasterConfig.h"
#include "RasterResource.h"
#include "RasterTool.h"
#include "rendergraph/RenderGraph.h"

#include <string_view>

namespace Moer::Render::Raster {

class SmaaEdgeDetectionPipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(SmaaEdgeDetectionPipeline);
    DEFINE_SHADER_CONSTANT(SmaaSharedPipelineBindlessParam, param);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_ARGS(bdls, param);
};
class SmaaBlendingWeightPipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(SmaaBlendingWeightPipeline);
    DEFINE_SHADER_CONSTANT(SmaaSharedPipelineBindlessParam, param);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_ARGS(bdls, param);
};
class SmaaNeighborhoodBlendingPipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(SmaaNeighborhoodBlendingPipeline);
    DEFINE_SHADER_CONSTANT(SmaaSharedPipelineBindlessParam, param);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_ARGS(bdls, param);
};
class SmaaT2xNeighborhoodBlendingPipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(SmaaT2xNeighborhoodBlendingPipeline);
    DEFINE_SHADER_CONSTANT(SmaaSharedPipelineBindlessParam, param);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_ARGS(bdls, param);
};
class SmaaT2xResolvePipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(SmaaT2xResolvePipeline);
    DEFINE_SHADER_CONSTANT(SmaaSharedPipelineBindlessParam, param);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_ARGS(bdls, param);
};

class FxaaPrecomputePipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(FxaaPrecomputePipeline);
    DEFINE_SHADER_CONSTANT(FxaaPrecomputePipelineBindlessParam, param);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_ARGS(bdls, param);
};

class FxaaPipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(FxaaPipeline);
    DEFINE_SHADER_CONSTANT(FxaaPipelineBindlessParam, param);
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
 *   注：SMAA T2x需要启用Reprojection才可以防止ghosting。Reprojection需要一个velocityTex，在这里我直接将velocityTex写入edgesTex的后两个通道
 *   注2：实际上，因为目前帧数为500+fps，所以看不到ghosting；可以在主循环中sleep 0.1s并且设置shader中的SMAAReprojection为0来得到一个ghosting的结果
 * 
 * 关于不同抗锯齿模式的说明
 *   切换抗锯齿时，多余的Pass不会被执行，应该不会有额外的性能开销
 * 
 * 关于SMAA实现的一些说明
 *   SMAA是通过直接集成论文仓库中的代码实现的（https://github.com/iryoku/smaa）
 *   原始代码不兼容bindless rhi，所以我将仓库中原始的代码封装了一下，并从bindless rhi中提取出了texture和sampler
 *   这部分可能破坏rhi的一些封装，具体见下面的GetSamplerIdx函数，除了这一点外，c++部分没有其他不优雅的代码
 *   shader部分和bindless rhi的耦合性特别高，如果修改bindless框架的话，大概率shader也要一起修改
 * Imporant: 所以如果修改了bindless框架，然后画面黑屏的话，请先将抗锯齿设置为FXAA(aa_mode = 2)，可以快速解决问题
 * 
 * 关于SMAA T2x的说明
 *   1. T2x使用了Temporal Supersampling，需要让相机抖动。可以通过camera->SetJitteredMatrix()来设置JitteredMatrix，这个矩阵会作用在ViewMatrix上
 *   2. 目前SMAA T2x效果和SMAA 1x类似，没有明显优势；不确定是场景问题还是实现问题
 *   FIXME: fix jitter in SMAA T2x when SMAA_REPROJECTION is enabled
 */
class AaPass {
public:
    struct GraphResources {
        RenderGraph::TextureHandle input{};
        RenderGraph::TextureHandle depth{};
        RenderGraph::TextureHandle aa_output{};
    };

    struct RecordParameters {
        EAaMode                              mode{EAaMode::NONE};
        BindlessArrayRef                     bindless{};
        TextureWithHandle                    input{};
        TextureWithHandle                    output{};
        TextureWithHandle                    texture_1{};
        TextureWithHandle                    texture_2{};
        TextureWithHandle                    temporal_output{};
        FxaaPrecomputePipelineBindlessParam  fxaa_precompute{};
        FxaaPipelineBindlessParam            fxaa{};
        SmaaSharedPipelineBindlessParam       smaa{};
    };

    AaPass(RasterContext& context) {

        auto& tex    = context.textures;
        auto  format = tex.aa_output.tex->GetFormat();

        assert(format == tex.aa_texture_1.tex->GetFormat());
        assert(format == tex.aa_texture_2.tex->GetFormat());
        assert(format == tex.aa_texture_3.tex->GetFormat());

        const auto create_fullscreen_pipeline = [&]<typename Pipeline>(
                                                    Pipeline&        pipeline,
                                                    std::string_view vertex_path,
                                                    std::string_view pixel_path,
                                                    std::string_view vertex_entry = "main",
                                                    std::string_view pixel_entry  = "main"
                                                ) {
            GfxPsoCreateInfo pso_full_screen_info(
                RHIRasterizeInfo::Preset(), {}, {RHIColorAttachmentInfo::Preset(format)}
            );
            pipeline = context.manager.Raster()
                           .Vertex(vertex_path, vertex_entry)
                           .Pixel(pixel_path, pixel_entry)
                           .Build<Pipeline>(std::move(pso_full_screen_info));
        };

        // smaa
        constexpr std::string_view smaa_path = "pipelines/postprocess/aa/SmaaWrapper.hlsl";
        create_fullscreen_pipeline(
            smaa_edge_detection_pipeline, smaa_path, smaa_path, "SMAAEdgeDetectionVS_Wrapper", "SMAALumaEdgeDetectionPS_Wrapper"
        );
        create_fullscreen_pipeline(
            smaa_blending_weight_pipeline, smaa_path, smaa_path, "SMAABlendingWeightCalculationVS_Wrapper", "SMAABlendingWeightCalculationPS_Wrapper"
        );
        create_fullscreen_pipeline(
            smaa_neighborhood_blending_pipeline, smaa_path, smaa_path, "SMAANeighborhoodBlendingVS_Wrapper", "SMAANeighborhoodBlendingPS_Wrapper"
        );
        create_fullscreen_pipeline(
            smaa_t2x_neighborhood_blending_pipeline, smaa_path, smaa_path, "SMAANeighborhoodBlendingVS_Wrapper", "SMAANeighborhoodBlendingPS_Wrapper"
        );
        create_fullscreen_pipeline(
            smaa_t2x_resolve_pipeline, smaa_path, smaa_path, "SMAAResolveVS_Wrapper", "SMAAResolvePS_Wrapper"
        );

        // fxaa
        create_fullscreen_pipeline(
            fxaa_precompute_pipeline, "core/utils/FullScreenQuad.hlsl", "pipelines/postprocess/aa/FxaaPrecompute.hlsl"
        );
        create_fullscreen_pipeline(
            fxaa_pipeline, "core/utils/FullScreenQuad.hlsl", "pipelines/postprocess/aa/Fxaa.hlsl"
        );

        // Other resources initialization
        aa_texture_34[0] = &tex.aa_texture_3;
        aa_texture_34[1] = &tex.aa_texture_4;
        ResetHistory();

        LoadSmaaResources(context);
    }

    void ResetHistory() {
        frame_parity          = 0;
        history_valid         = false;
        current_view_proj     = Matrix4x4f::Identity();
    }

    [[nodiscard]] uint8 NextSmaaT2xPhase() const noexcept {
        return history_valid ?
                   static_cast<uint8>(frame_parity ^ 1u) :
                   0u;
    }

    void LoadSmaaResources(RasterContext& context) {
        Sampler linear_sampler(SF_LINEAR, SAM_REPEAT);

        // smaa area tex
        smaa_area_tex.tex = context.device.CreateTexture(
            "smaa_area_tex",
            Extent2D(SMAA_AREATEX_WIDTH, SMAA_AREATEX_HEIGHT),
            PF_R8G8_UNORM,
            ETextureUsageFlags::SAMPLED | ETextureUsageFlags::TRANSFER_DST
        );
        RecordNewGraphicsTextureUpload(
            context.cmd_list,
            std::span<byte>(
                (byte*)&SmaaPrecomputedTextures::areaTexBytes, sizeof(SmaaPrecomputedTextures::areaTexBytes)
            ),
            smaa_area_tex.tex
        );
        smaa_area_tex.hdl = context.bdls->AllocateTexture(smaa_area_tex.tex, linear_sampler);

        // smaa search tex
        smaa_search_tex.tex = context.device.CreateTexture(
            "smaa_search_tex",
            Extent2D(SMAA_SEARCHTEX_WIDTH, SMAA_SEARCHTEX_HEIGHT),
            PF_R8_UNORM,
            ETextureUsageFlags::SAMPLED | ETextureUsageFlags::TRANSFER_DST
        );
        RecordNewGraphicsTextureUpload(
            context.cmd_list,
            std::span<byte>(
                (byte*)&SmaaPrecomputedTextures::searchTexBytes,
                sizeof(SmaaPrecomputedTextures::searchTexBytes)
            ),
            smaa_search_tex.tex
        );
        smaa_search_tex.hdl = context.bdls->AllocateTexture(smaa_search_tex.tex, linear_sampler);
    }

    [[nodiscard]] RecordParameters Prepare(
        const RasterContext& context,
        const RasterConfig&  ui_config,
        const Camera&        camera,
        TextureWithHandle    input_image,
        uint8                smaa_t2x_phase
    ) const {
        RecordParameters parameters{};
        parameters.mode      = ui_config.aa_mode;
        parameters.bindless  = context.bdls;
        parameters.input     = std::move(input_image);
        parameters.output    = context.textures.aa_output;
        parameters.texture_1 = context.textures.aa_texture_1;
        parameters.texture_2 = context.textures.aa_texture_2;

        if (ui_config.aa_mode == EAaMode::NONE ||
            ui_config.aa_mode == EAaMode::FXAA_SIMPLIFIED ||
            ui_config.aa_mode == EAaMode::FXAA_QUALITY) {
            parameters.fxaa_precompute.input_image = parameters.input.hdl;
            parameters.fxaa.input_image            = parameters.texture_1.hdl;
            parameters.fxaa.fxaa_mode              = static_cast<uint32>(ui_config.aa_mode);
            parameters.fxaa.resolution             = float2(parameters.output.GetSize());
            parameters.fxaa.inv_resolution = float2(1.0) / parameters.fxaa.resolution;
            return parameters;
        }

        assert(
            (ui_config.aa_mode == EAaMode::SMAA_1X ||
             ui_config.aa_mode == EAaMode::SMAA_T2X) &&
            "Invalid antialiasing mode"
        );

        // TODO: optimize the following code
        //           以下是我会写出这段代码的原因：
        //       SMAA官方提供了一段代码SMAA.hlsl，只需要一些简单的修改，就可以让我们快速将SMAA集成到MoerEngine中
        //       但是SMAA.hlsl并不支持我们的bindless后的rhi，所以我修改了SMAA.hlsl，并试图提取出了独立的texture和sampler
        //       因此，我需要texture index和sampler index
        //       texture index直接使用context.bdls->AllocateTexture就可以解决
        //       sampler index是通过VulkanDevice::GetSamplerIndex()得到的，但这个函数所在的头文件并不能被include（因为不位于include目录下）
        //       那么为了获取sampler index，我们有两种方案 1. 复制GetSamplerIdx的实现；2. 通过AllocateTexture得到handle后再解压出sampler index
        //       第一种方案可能导致代码不一致，而第二种方案过于不可控，而且我不确定是否有潜在的性能开销，所以我同时写了两种方案，并且写下了这段说明
        //       目前使用第一种
        //       有更好的方案的话，直接修改下面这段代码即可
        auto GetSamplerIdx = [&](const Sampler& sampler) {
            // method 1
            uint32 filter  = uint32(sampler.filter);
            uint32 address = uint32(sampler.address_mode);
            uint32 compare = uint32(sampler.compare_function);
            return (uint32(SF_Num) * uint32(SAM_Num)) * compare + (uint32(SF_Num)) * address + filter;
            // method 2
            // uint bdls_tex_handle = context.bdls->AllocateTexture(antialiasing_output, sampler);
            // uint sampler_idx     = bdls_tex_handle & 0xff;
            // return sampler_idx;
        };

        const bool uses_temporal_history = ui_config.aa_mode == EAaMode::SMAA_T2X;
        const bool had_valid_history =
            uses_temporal_history && history_valid;
        const Matrix4x4f previous =
            had_valid_history ? current_view_proj : camera.GetViewProjectionMatrix();
        const uint8 phase = uses_temporal_history ? smaa_t2x_phase : 0u;
        assert(phase < aa_texture_34.size());
        parameters.temporal_output       = *aa_texture_34[phase];

        parameters.smaa = [&]() {
            SmaaSharedPipelineBindlessParam param{};
            param.clip2world         = Transpose(camera.GetViewProjectionMatrixInv());
            param.aa_mode            = static_cast<uint32>(ui_config.aa_mode);
            param.color_tex          = parameters.input.hdl;
            param.depth_tex          = context.textures.depth_linear_sampler.hdl;
            param.search_tex         = smaa_search_tex.hdl;
            param.area_tex           = smaa_area_tex.hdl;
            param.edges_tex          = parameters.texture_1.hdl;
            param.blend_tex          = parameters.texture_2.hdl;
            param.current_color_tex  = parameters.temporal_output.hdl;
            param.previous_color_tex =
                aa_texture_34[
                    had_valid_history ? static_cast<uint8>(phase ^ 1u) : phase
                ]->hdl;
            param.frame_index        = phase;
            param.point_sampler      = GetSamplerIdx(Sampler(SF_NEAREST, SAM_CLAMP_TO_EDGE));
            param.linear_sampler     = GetSamplerIdx(Sampler(SF_LINEAR, SAM_CLAMP_TO_EDGE));
            param.rt_metrics         = float4(
                1.0f / context.textures.aa_output.GetSizeX(),
                1.0f / context.textures.aa_output.GetSizeY(),
                context.textures.aa_output.GetSizeX(),
                context.textures.aa_output.GetSizeY()
            );
            param.clip2prev_clip = Transpose(previous * camera.GetViewProjectionMatrixInv());
            return param;
        }();

        return parameters;
    }

    void RecordEdgesOrLuma(CommandList& cmd_list, const RecordParameters& parameters) {
        if (parameters.mode <= EAaMode::FXAA_QUALITY) {
            cmd_list.Gfx(fxaa_precompute_pipeline, parameters.bindless, parameters.fxaa_precompute)
                .Draw("FXAA Precompute Pass", parameters.texture_1.GetRect2D(),
                      RasterTool::GetFullScreenDrawDatas(), ColorAttachment(parameters.texture_1.tex));
        } else {
            cmd_list.Gfx(smaa_edge_detection_pipeline, parameters.bindless, parameters.smaa)
                .Draw("SMAA Edge Detection Pass", parameters.texture_1.GetRect2D(),
                      RasterTool::GetFullScreenDrawDatas(), ColorAttachment(parameters.texture_1.tex));
        }
    }

    void RecordWeights(CommandList& cmd_list, const RecordParameters& parameters) {
        cmd_list.Gfx(smaa_blending_weight_pipeline, parameters.bindless, parameters.smaa)
            .Draw("SMAA Blending Weight Calculation Pass", parameters.texture_2.GetRect2D(),
                  RasterTool::GetFullScreenDrawDatas(), ColorAttachment(parameters.texture_2.tex));
    }

    void RecordBlend(CommandList& cmd_list, const RecordParameters& parameters) {
        if (parameters.mode <= EAaMode::FXAA_QUALITY) {
            cmd_list.Gfx(fxaa_pipeline, parameters.bindless, parameters.fxaa)
                .Draw("FXAA Pass", parameters.output.GetRect2D(), RasterTool::GetFullScreenDrawDatas(),
                      ColorAttachment(parameters.output.tex));
        } else if (parameters.mode == EAaMode::SMAA_1X) {
            cmd_list.Gfx(smaa_neighborhood_blending_pipeline, parameters.bindless, parameters.smaa)
                .Draw("SMAA Neighborhood Blending Pass", parameters.output.GetRect2D(),
                      RasterTool::GetFullScreenDrawDatas(), ColorAttachment(parameters.output.tex));
        } else {
            cmd_list.Gfx(smaa_t2x_neighborhood_blending_pipeline, parameters.bindless, parameters.smaa)
                .Draw("SMAA T2x Neighborhood Blending Pass", parameters.temporal_output.GetRect2D(),
                      RasterTool::GetFullScreenDrawDatas(), ColorAttachment(parameters.temporal_output.tex));
        }
    }

    void RecordResolve(CommandList& cmd_list, const RecordParameters& parameters) {
        cmd_list.Gfx(smaa_t2x_resolve_pipeline, parameters.bindless, parameters.smaa)
            .Draw("SMAA T2x Resolve Pass", parameters.output.GetRect2D(), RasterTool::GetFullScreenDrawDatas(),
                  ColorAttachment(parameters.output.tex));
    }

    void AddToGraph(
        RenderGraph& graph,
        const RasterContext& context,
        const RasterConfig& ui_config,
        const Camera& camera,
        TextureWithHandle input_image,
        uint8 smaa_phase,
        GraphResources resources
    );

    void CommitFrame(EAaMode mode, uint8 smaa_t2x_phase, const Matrix4x4f& view_proj) {
        history_valid = mode == EAaMode::SMAA_T2X;
        frame_parity = history_valid ? smaa_t2x_phase : 0u;
        current_view_proj = history_valid ? view_proj : Matrix4x4f::Identity();
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
    bool                               history_valid = false;
    Matrix4x4f                         current_view_proj = Matrix4x4f::Identity();

    TextureWithHandle smaa_area_tex;
    TextureWithHandle smaa_search_tex;

    // fxaa
    FxaaPrecomputePipeline fxaa_precompute_pipeline;
    FxaaPipeline           fxaa_pipeline;
};

} // namespace Moer::Render::Raster
