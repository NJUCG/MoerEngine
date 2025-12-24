#pragma once

#include "math/Function.h"
#include "scene/Camera.h"
#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/raster/post_process/ShaderParameters.h"

#include "RasterConfig.h"
#include "RasterResource.h"
#include "RasterTool.h"

namespace Moer::Render::Raster {

// 注意，ComputePipeline不支持隐式LOD采样，所以没法使用Bindless
class TonemappingHistogramPipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(TonemappingHistogramPipeline);

    DEFINE_SHADER_CONSTANT_STRUCT(TonemappingPipelineBindlessParam, param);
    DEFINE_SHADER_TEX(input_image);  // input
    DEFINE_SHADER_BUFFER(histogram); // output

    DEFINE_SHADER_ARGS(param, input_image, histogram);
};

class TonemappingExposurePipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(TonemappingExposurePipeline);

    DEFINE_SHADER_CONSTANT_STRUCT(TonemappingPipelineBindlessParam, param);
    DEFINE_SHADER_BUFFER(histogram); // input
    DEFINE_SHADER_BUFFER(exposure);  // output

    DEFINE_SHADER_ARGS(param, histogram, exposure);
};

class TonemappingPipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(TonemappingPipeline);

    DEFINE_SHADER_CONSTANT_STRUCT(TonemappingPipelineBindlessParam, param);
    DEFINE_SHADER_TEX(input_image); // input
    DEFINE_SHADER_BUFFER(exposure); // input

    DEFINE_SHADER_ARGS(param, input_image, exposure);
};

/**
 * MARK: Tonemapping Pass
 * 
 * 注：Gamma矫正使用硬件sRGB实现，不需要在Shader中手动进行Gamma矫正
 * 
 * 创建光源或者实现曝光时，请参考 光源强度单位参考表
 * - 详见 source\runtime\render\scene\light\LightComponent.cpp: LightComponent::CreateDefaultLightComponents
 */
class TonemappingPass {
public:
    TonemappingPass(RasterContext& context) {
        GfxPsoCreateInfo pso_full_screen_info(
            RHIRasterizeInfo::Preset(),
            {},
            {RHIColorAttachmentInfo::Preset(context.textures.tonemapping_output.tex->GetFormat())}
        );

        // Pipeline
        tonemapping_pipeline = context.manager.Raster()
                                   .Vertex("core/utils/FullScreenQuad.hlsl")
                                   .Pixel("pipelines/postprocess/color/Tonemapping.hlsl")
                                   .Build<TonemappingPipeline>(std::move(pso_full_screen_info));

        tonemapping_histogram_pipeline = context.manager.Compute<TonemappingHistogramPipeline>(
            "pipelines/postprocess/color/TonemappingHistogram.hlsl"
        );

        tonemapping_exposure_pipeline = context.manager.Compute<TonemappingExposurePipeline>(
            "pipelines/postprocess/color/TonemappingExposure.hlsl"
        );

        // Buffer
        histogram_buffer = context.device.CreateBuffer<uint>(
            "raster histogram buffer", TONEMAPPING_HISTOGRAM_BIN_COUNT, EBufferUsageFlags::UNORDERED_ACCESS
        );
        exposure_buffer = context.device.CreateBuffer<uint>(
            "raster exposure buffer", 1, EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::CPU_VISIBLE
        );
    }

    TextureWithHandle
    Process(RasterContext& context, const RasterConfig& ui_config, TextureWithHandle input_image) {

        uint2 input_res = input_image.GetSize();

        TonemappingPipelineBindlessParam param;
        {
            param.resolution     = input_res;
            param.resolution_inv = float2(1.f / input_res.x, 1.f / input_res.y);

            // 这两个参数用于将log2x in [min, max]映射到 log2x in [0, 1]
            param.log2lum_saturate_scale = 1.0f / (ui_config.tonemapping_exposure_log2lum_max -
                                                   ui_config.tonemapping_exposure_log2lum_min);
            param.log2lum_saturate_bias =
                -ui_config.tonemapping_exposure_log2lum_min * param.log2lum_saturate_scale;

            param.exposure_ev      = ui_config.tonemapping_exposure_ev;
            param.reinhard_enabled = ui_config.tonemapping_reinhard_enabled ? 1 : 0;

            param.debug_param = ui_config.debug_param;
        }

        // Reset
        {
            context.cmd_list.ClearResource(histogram_buffer->GetView(), 0);
            context.cmd_list.ClearResource(exposure_buffer->GetView(), 0);
        }

        // Histogram Pass
        {
            context.cmd_list.Compute(tonemapping_histogram_pipeline, param, input_image.tex, histogram_buffer)
                .Dispatch(
                    uint3(
                        (input_res.x - 1) / TONEMAPPING_HISTOGRAM_GROUP_X + 1,
                        (input_res.y - 1) / TONEMAPPING_HISTOGRAM_GROUP_Y + 1,
                        1
                    ),
                    "Tonemapping Histogram Pass"
                );
        }

        // Exposure Pass
        {
            context.cmd_list.Compute(tonemapping_exposure_pipeline, param, histogram_buffer, exposure_buffer)
                .Dispatch(uint3(1, 1, 1), "Tonemapping Exposure Pass");
        }

        // Tonemapping Pass
        {
            context.cmd_list.Gfx(tonemapping_pipeline, param, input_image.tex, exposure_buffer)
                .Draw(
                    "Tonemapping Pass",
                    context.textures.tonemapping_output.GetRect2D(),
                    std::move(RasterTool::GetFullScreenDrawDatas()),
                    ColorAttachment(context.textures.tonemapping_output.tex)
                );
        }

        return context.textures.tonemapping_output;
    }

private:
    TonemappingPipeline          tonemapping_pipeline;
    TonemappingHistogramPipeline tonemapping_histogram_pipeline;
    TonemappingExposurePipeline  tonemapping_exposure_pipeline;

    BufferRef histogram_buffer;
    BufferRef exposure_buffer;
};

} // namespace Moer::Render::Raster