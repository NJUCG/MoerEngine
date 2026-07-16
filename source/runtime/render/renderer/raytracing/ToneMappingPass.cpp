#include "ToneMappingPass.h"

// 实现基于直方图的自动曝光，并执行全屏 Tone Mapping。

#include "PixelFormat.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "shader/ShaderResourceManager.h"
#include "shaderheaders/shared/postprocess/ShaderParameters.h"

#include <algorithm>
#include <cmath>

namespace Moer::Render::Raytracing {

namespace {

constexpr float g_min_log_luminance = -10.f;
constexpr float g_max_log_luminance = 4.f;

} // namespace

ToneMappingPass::ToneMappingPass(RenderDevice& device, ShaderManager& manager, CreateInfo info) :
    histogram_pipeline{manager.Compute<HistogramPipeline>("pipelines/raytracing/postprocess/Histogram.hlsl")},
    exposure_pipeline{manager.Compute<ExposurePipeline>("pipelines/raytracing/postprocess/Exposure.hlsl")} {

    VertexStream     stream{};
    GfxPsoCreateInfo pso_info(
        RHIRasterizeInfo::Preset(), stream, {RHIColorAttachmentInfo::Preset(PF_R8G8B8A8_UNORM)}
    );

    tone_mapping_pass_pipeline = manager.Raster()
                                     .Vertex("core/utils/FullScreenQuad.hlsl")
                                     .Pixel("pipelines/raytracing/postprocess/ToneMappingPass.hlsl")
                                     .Build<ToneMappingPassPipeline>(std::move(pso_info));

    tone_mapping_constants = device.CreateBuffer<Moer::byte>(
        "tone_mapping_constants", sizeof(ToneMappingParams), EBufferUsageFlags::CONSTANT_BUFFER
    );
    histogram_buffer = device.CreateBuffer<uint>(
        "histogram_buffer",
        info.histogram_bins,
        EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::TEXTURE_BUFFER
    );
    histogram_buffer->SetName("histogram_buffer");
    exposure_buffer = device.CreateBuffer<uint>(
        "exposure_buffer", 1, EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::TEXTURE_BUFFER
    );

    if (info.color_lut) {
        color_lut = info.color_lut;
        if (color_lut->GetHeight() * color_lut->GetHeight() != color_lut->GetWidth()) {
            color_lut_size = 0;
        }
    }
}

void ToneMappingPass::Process(
    CommandList& cmd_list,
    Params       pass_params,
    TextureRef   source_texture,
    TextureRef   target_texture
) {
    const bool enable_color_lut = pass_params.enable_color_lut && color_lut_size > 0;

    cmd_list.PushScopeWithTimeScope("ToneMappingPass");
    ToneMappingParams params{};
    params.log_luminance_scale          = 1.f / (g_max_log_luminance - g_min_log_luminance);
    params.log_luminance_bias           = -g_min_log_luminance * params.log_luminance_scale;
    params.log_luminance_scale_exposure = g_max_log_luminance - g_min_log_luminance;
    params.log_luminance_bias_exposure  = g_min_log_luminance;
    params.histogram_low_percentile =
        std::min(0.99f, std::max(0.f, pass_params.histogram_low_percentile));
    params.histogram_high_percentile = std::min(
        1.f, std::max(pass_params.histogram_high_percentile, pass_params.histogram_low_percentile)
    );
    params.eye_adaptation_speed_up   = pass_params.eye_adaptation_speed_up;
    params.eye_adaptation_speed_down = pass_params.eye_adaptation_speed_down;
    params.min_adapted_luminance     = pass_params.min_adapted_luminance;
    params.max_adapted_luminance     = pass_params.max_adapted_luminance;
    params.frame_time                = frame_time;
    params.view_origin               = uint2(0, 0);
    params.view_size                 = uint2(source_texture->GetExtent().x, source_texture->GetExtent().y);
    params.exposure_scale            = std::exp2f(pass_params.exposure_bias);
    params.white_point_inv_squared   = 1.f / (pass_params.white_point * pass_params.white_point);
    params.source_slice              = 0;
    params.color_lut_size = enable_color_lut ?
                                float2(color_lut_size * color_lut_size, color_lut_size) :
                                float2(0.f);
    params.color_lut_size_inv = enable_color_lut ?
                                    float2(
                                        1.f / (color_lut_size * color_lut_size), 1.f / color_lut_size
                                    ) :
                                    float2(0.f);
    params.frame_idx = frame_idx;
    params.enabled   = pass_params.enable_tone_mapping ? 1 : 0;

    if (!tone_mapping_enabled != pass_params.enable_tone_mapping) {
        tone_mapping_enabled = pass_params.enable_tone_mapping;
        ResetExposure(cmd_list);
    }

    upload_data.resize(sizeof(ToneMappingParams));
    upload_data.assign(
        reinterpret_cast<byte*>(&params), reinterpret_cast<byte*>(&params) + sizeof(ToneMappingParams)
    );
    cmd_list.CopyFrom(std::move(upload_data), tone_mapping_constants->GetView());

    ResetHistogram(cmd_list);
    ComputeHistogram(cmd_list, source_texture);
    ComputeExposure(cmd_list);
    Render(cmd_list, source_texture, target_texture);
    cmd_list.PopScopeWithTimeScope();
}

void ToneMappingPass::Render(CommandList& cmd_list, TextureRef source_texture, TextureRef target_texture) {
    Array<SingleDrawParam> full_screen_draw_data{SingleDrawParam{3, 1, 0, 0, 0}};
    Sampler               linear_clamp_sampler{SF_LINEAR, SAM_CLAMP_TO_EDGE};
    cmd_list
        .Gfx(
            tone_mapping_pass_pipeline,
            tone_mapping_constants,
            source_texture,
            exposure_buffer,
            color_lut,
            linear_clamp_sampler
        )
        .Draw(
            "ToneMapping",
            Rect2D(0, 0, target_texture->GetExtent().x, target_texture->GetExtent().y),
            std::move(full_screen_draw_data),
            ColorAttachment(target_texture)
        );
}

void ToneMappingPass::ResetHistogram(CommandList& cmd_list) {
    cmd_list.ClearResource(histogram_buffer->GetView(), 0);
}

void ToneMappingPass::ResetExposure(CommandList& cmd_list) {
    cmd_list.ClearResource(exposure_buffer->GetView(), 0);
}

void ToneMappingPass::ComputeHistogram(CommandList& cmd_list, TextureRef source_texture) {
    cmd_list.Compute(histogram_pipeline, tone_mapping_constants, source_texture, histogram_buffer)
        .Dispatch(
            uint3((source_texture->GetExtent().x + 15) / 16, (source_texture->GetExtent().y + 15) / 16, 1),
            "Calculate Histogram"
        );
}

void ToneMappingPass::ComputeExposure(CommandList& cmd_list) {
    cmd_list.Compute(exposure_pipeline, tone_mapping_constants, histogram_buffer, exposure_buffer)
        .Dispatch(uint3(1, 1, 1), "Calculate Exposure");
}

void ToneMappingPass::AdvanceFrame(float elapsed_time) {
    frame_time = elapsed_time;
    ++frame_idx;
}

} // namespace Moer::Render::Raytracing
