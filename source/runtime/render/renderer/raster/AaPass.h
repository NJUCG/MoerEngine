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
            "smaa_area_tex",
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
            "smaa_search_tex",
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
            .Draw(
                "FXAA Precompute Pass",
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
            .Draw(
                "FXAA Pass",
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
        // TODO: optimize the following code
        //           以下是我会写出这段代码的原因：
        //       SMAA官方提供了一段代码SMAA.hlsl，只需要一些简单的修改，就可以让我们快速将SMAA集成到MoerEngine中
        //       但是SMAA.hlsl并不支持我们的bindless后的rhi，所以我修改了SMAA.hlsl，并试图提取出了独立的texture和sampler
        //       因此，我需要texture index和sampler index
        //       texture index直接使用context.bdls->AllocateTexture就可以解决
        //       sampler index是通过VulkanDevice::GetSamplerIdx()得到的，但这个函数所在的头文件并不能被include（因为不位于include目录下）
        //       那么为了获取sampler index，我们有两种方案 1. 复制GetSamplerIdx的实现；2. 通过AllocateTexture得到handle后再解压出sampler index
        //       第一种方案可能导致代码不一致，而第二种方案过于不可控，而且我不确定是否有潜在的性能开销，所以我同时写了两种方案，并且写下了这段说明
        //       目前使用第一种
        //       有更好的方案的话，直接修改下面这段代码即可
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
            param.aa_mode            = static_cast<uint32>(ui_config.aa_mode);
            param.color_tex          = input_image.hdl;
            param.position_tex       = context.textures.position.hdl;
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
            param.curr_inv_vp_and_prev_vp = Transpose(previous_view_proj * current_inv_view_proj);
            return param;
        }();

        context.cmd_list.Gfx(smaa_edge_detection_pipeline, context.bdls, smaa_shared_param)
            .Draw(
                "SMAA Edge Detection Pass",
                context.textures.aa_texture_1.GetRect2D(),
                std::move(RasterTool::GetFullScreenDrawDatas()),
                ColorAttachment(context.textures.aa_texture_1.tex)
            );

        context.cmd_list.Gfx(smaa_blending_weight_pipeline, context.bdls, smaa_shared_param)
            .Draw(
                "SMAA Blending Weight Calculation Pass",
                context.textures.aa_texture_2.GetRect2D(),
                std::move(RasterTool::GetFullScreenDrawDatas()),
                ColorAttachment(context.textures.aa_texture_2.tex)
            );

        if (ui_config.aa_mode == EAaMode::SMAA_1X) {
            context.cmd_list.Gfx(smaa_neighborhood_blending_pipeline, context.bdls, smaa_shared_param)
                .Draw(
                    "SMAA Neighborhood Blending Pass",
                    context.textures.aa_output.GetRect2D(),
                    std::move(RasterTool::GetFullScreenDrawDatas()),
                    ColorAttachment(context.textures.aa_output.tex)
                );
        } else if (ui_config.aa_mode == EAaMode::SMAA_T2X) {
            context.cmd_list.Gfx(smaa_t2x_neighborhood_blending_pipeline, context.bdls, smaa_shared_param)
                .Draw(
                    "SMAA T2x Neighborhood Blending Pass",
                    context.textures.aa_texture_3.GetRect2D(),
                    std::move(RasterTool::GetFullScreenDrawDatas()),
                    ColorAttachment(aa_texture_34[frame_parity]->tex)
                );

            context.cmd_list.Gfx(smaa_t2x_resolve_pipeline, context.bdls, smaa_shared_param)
                .Draw(
                    "SMAA T2x Resolve Pass",
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