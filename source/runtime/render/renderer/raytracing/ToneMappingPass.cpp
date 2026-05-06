#include "ToneMappingPass.h"

#include "PixelFormat.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "rhi/RHIResourceInitilizer.h"
#include "shader/ShaderResourceManager.h"
#include "shaderheaders/shared/postprocess/ShaderParameters.h"
#include <winscard.h>

namespace Moer::Render::Raytracing {

ToneMappingPass::ToneMappingPass(
    RenderDevice&  _device,
    ShaderManager& _manager,
    Scene&         _scene,
    CreateInfo     _info
) :
    device(_device),
    manager(_manager),
    histogram_pipeline{manager.Compute<HistogramPipeline>("pipelines/raytracing/postprocess/Histogram.hlsl")},
    exposure_pipeline{manager.Compute<ExposurePipeline>("pipelines/raytracing/postprocess/Exposure.hlsl")},
    scene(_scene) {

    VertexStream     stream{};
    GfxPsoCreateInfo pso_info(
        RHIRasterizeInfo::Preset(), stream, {RHIColorAttachmentInfo::Preset(PF_R8G8B8A8_UNORM)}
    );

    tone_mapping_pass_pipeline = manager.Raster()
                                     .Vertex("core/utils/FullScreenQuad.hlsl")
                                     .Pixel("pipelines/raytracing/postprocess/ToneMappingPass.hlsl")
                                     .Build<ToneMappingPassPipeline>(std::move(pso_info));

    tone_mapping_constants = device.CreateBuffer<Moer::byte>(
        MOER_TEXT("tone_mapping_constants"), sizeof(ToneMappingParams), EBufferUsageFlags::CONSTANT_BUFFER
    );
    tone_mapping_constants2 = device.CreateBuffer<Moer::byte>(
        MOER_TEXT("tone_mapping_constants2"), sizeof(ToneMappingParams), EBufferUsageFlags::CONSTANT_BUFFER
    );
    histogram_buffer = device.CreateBuffer<uint>(
        MOER_TEXT("histogram_buffer"), _info.histogram_bins, EBufferUsageFlags::UNORDERED_ACCESS
    );
    histogram_buffer->SetName(MOER_TEXT("histogram_buffer"));
    exposure_buffer = device.CreateBuffer<uint>(MOER_TEXT("exposure_buffer"), 1, EBufferUsageFlags::UNORDERED_ACCESS);

    if (_info.color_lut) {
        color_lut = _info.color_lut;
        if (color_lut->GetHeight() * color_lut->GetHeight() != color_lut->GetWidth()) {
            //
            color_lut_size = 0;
        }
    }

    indirect_buffer = device.CreateBuffer<byte>(
        MOER_TEXT("ToneMapping::IndirectBuffer"),
        sizeof(DrawCmdData) + sizeof(DrawIndexedCmdData),
        EBufferUsageFlags::INDIRECT_BUFFER
    );
    count_buffer =
        device.CreateBuffer<uint>(MOER_TEXT("ToneMapping::CountBuffer"), 1, EBufferUsageFlags::INDIRECT_BUFFER);
    index_buffer = device.CreateBuffer<uint>(MOER_TEXT("ToneMapping::IndexBuffer"), 3, EBufferUsageFlags::INDEX_BUFFER);
}
static constexpr float g_min_log_luminance = -10; // TODO: figure out how to set these properly
static constexpr float g_max_log_luminamce = 4;

void ToneMappingPass::Process(
    CommandList& _cmd_list,
    RTContext&   _rt_ctx,
    Params       _params,
    TextureRef   _src_tex,
    TextureRef   _target
) {
    bool b_enable_lut = _params.enable_color_lut && color_lut_size > 0;

    _cmd_list.PushScopeWithTimeScope(MOER_TEXT("ToneMappingPass"));
    ToneMappingParams params{};
    params.log_luminance_scale          = 1.f / (g_max_log_luminamce - g_min_log_luminance);
    params.log_luminance_bias           = -g_min_log_luminance * params.log_luminance_scale;
    params.log_luminance_scale_exposure = g_max_log_luminamce - g_min_log_luminance;
    params.log_luminance_bias_exposure  = g_min_log_luminance;
    params.histogram_low_percentile     = std::min(0.99f, std::max(0.f, _params.histogram_low_percentile));
    params.histogram_high_percentile =
        std::min(1.f, std::max(_params.histogram_high_percentile, _params.histogram_low_percentile));
    params.eye_adaptation_speed_up   = _params.eye_adaptation_speed_up;
    params.eye_adaptation_speed_down = _params.eye_adaptation_speed_down;
    params.min_adapted_luminance     = _params.min_adapted_luminance;
    params.max_adapted_luminance     = _params.max_adapted_luminance;
    params.frame_time                = frame_time;
    params.view_origin               = uint2(0, 0);
    params.view_size                 = uint2(_src_tex->GetExtent().x, _src_tex->GetExtent().y);
    params.exposure_scale            = std::exp2f(_params.exposure_bias);
    params.white_point_inv_squared   = 1.f / (_params.white_point * _params.white_point);
    params.source_slice              = 0;
    params.color_lut_size =
        b_enable_lut ? float2(color_lut_size * color_lut_size, color_lut_size) : float2(0.f);
    params.color_lut_size_inv =
        b_enable_lut ? float2(1.f / (color_lut_size * color_lut_size), 1.f / color_lut_size) : float2(0.f);
    params.frame_idx = frame_idx;
    params.enabled   = _params.enable_tone_mapping ? 1 : 0;

    if (!b_enabled != _params.enable_tone_mapping) {
        b_enabled = _params.enable_tone_mapping;
        ResetExposure(_cmd_list);
    }

    upload_data.resize(sizeof(ToneMappingParams));
    upload_data.assign((byte*)&params, (byte*)&params + sizeof(ToneMappingParams));

    _cmd_list.CopyFrom(std::move(upload_data), tone_mapping_constants->GetView());

    ResetHistogram(_cmd_list);
    ComputeHistogram(_cmd_list, _src_tex);
    ComputeExposure(_cmd_list, _params);
    Render(_cmd_list, _rt_ctx, _params, _src_tex, _target);
    _cmd_list.PopScopeWithTimeScope();
}

void ToneMappingPass::Render(
    CommandList& _cmd_list,
    RTContext&   _rt_ctx,
    Params       _params,
    TextureRef   _src_tex,
    TextureRef   _target
) {

    bool draw_indirect = false;

    if (draw_indirect) {
        DrawCmdData draw_cmd_data{};
        draw_cmd_data.first_vtx      = 0;
        draw_cmd_data.first_instance = 0;
        draw_cmd_data.instance_cnt   = 1;
        draw_cmd_data.vertex_cnt     = 3;

        DrawIndexedCmdData draw_indexed_cmd_data{};
        draw_indexed_cmd_data.first_index    = 0;
        draw_indexed_cmd_data.first_instance = 0;
        draw_indexed_cmd_data.instance_cnt   = 1;
        draw_indexed_cmd_data.vertex_offset  = 0;
        draw_indexed_cmd_data.index_cnt      = 3;

        upload_data.resize(sizeof(DrawCmdData));
        std::memcpy(upload_data.data(), &draw_cmd_data, sizeof(DrawCmdData));
        _cmd_list.CopyFrom(std::move(upload_data), indirect_buffer->GetView(0, sizeof(DrawCmdData)));

        upload_data.resize(sizeof(DrawIndexedCmdData));
        std::memcpy(upload_data.data(), &draw_indexed_cmd_data, sizeof(DrawIndexedCmdData));
        _cmd_list.CopyFrom(std::move(upload_data), indirect_buffer->GetView(sizeof(DrawCmdData)));

        /**
         * 注意！
         * 
         * 这里虽然把DrawCmdData和DrawIndexedCmdData都上传到了indirect_buffer
         * 但是后续的DrawIndirect调用时，只会使用其中一种结构体
         * 
         * DrawCmdData，不带索引的indirect绘制
         * DrawIndexedCmdData，带索引的indirect绘制
         * 
         * 关键是 indirect_buffer->GetView(偏移, 大小)
         * 
         * 比如下文，DrawIndexedIndirect，关键的 `GetView(sizeof(DrawCmdData))`
         * => 意味着使用DrawIndexedCmdData结构体
         * 
         * DrawIndirect，关键的 `GetView(0, sizeof(DrawCmdData))`
         * => 意味着使用DrawCmdData结构体
         * 
         * Keep indirect buffer view offsets aligned with the selected draw command structure.
         */

        uint count = 1;
        upload_data.resize(sizeof(uint));
        std::memcpy(upload_data.data(), &count, sizeof(uint));
        _cmd_list.CopyFrom(std::move(upload_data), count_buffer->GetView());

        Array<uint> indices{0, 1, 2};
        upload_data.resize(sizeof(uint) * 3);
        std::memcpy(upload_data.data(), indices.data(), sizeof(uint) * 3);
        _cmd_list.CopyFrom(std::move(upload_data), index_buffer->GetView());

        Sampler linear_clamp_sampler{SF_LINEAR, SAM_CLAMP_TO_EDGE};
        _cmd_list
            .Gfx(
                tone_mapping_pass_pipeline,
                tone_mapping_constants,
                _src_tex,
                exposure_buffer,
                color_lut,
                linear_clamp_sampler
            )
            ////////////////////////////////////////////// DrawIndexedIndirect using gpu counter buffer
            // .DrawIndirect(
            //     "ToneMapping",
            //     Rect2D(0, 0, _target->GetExtent().x, _target->GetExtent().y),
            //     {},
            //     IndexBuffer{index_buffer->GetView(), EIndexElementType::IET_UINT32},
            //     indirect_buffer->GetView(sizeof(DrawCmdData)),
            //     count_buffer->GetView(),
            //     1,
            //     sizeof(DrawIndexedCmdData),
            //     ColorAttachment(_target)
            // );
            ////////////////////////////////////////////// DrawIndirect without index using gpu counter buffer
            // .DrawIndirect(
            //     "ToneMapping",
            //     Rect2D(0, 0, _target->GetExtent().x, _target->GetExtent().y),
            //     {},
            //     0, //0 for draw, struct for draw indexed
            //     indirect_buffer->GetView(0, sizeof(DrawCmdData)),
            //     count_buffer->GetView(), //uint for count, BufferView for
            //     1,
            //     sizeof(DrawCmdData),
            //     ColorAttachment(_target)
            // );
            ////////////////////////////////////////////// DrawIndirect without index using cpu count
            .DrawIndirect(MOER_TEXT("ToneMapping"),
                Rect2D(0, 0, _target->GetExtent().x, _target->GetExtent().y),
                {}, // Vertex Buffer
                0,  // Vertex Count. 0 for draw, struct for draw indexed
                indirect_buffer->GetView(0, sizeof(DrawCmdData)), // Indirect Buffer
                1,                                                // Indirect Buffer Count, cpu count
                sizeof(DrawCmdData),                              // Indirect Buffer Stride
                ColorAttachment(_target)
            );
    } else {
        auto get_full_screen_draw_datas = [&]() {
            Array<SingleDrawParam> full_screen_draw_datas;
            full_screen_draw_datas.emplace_back(SingleDrawParam{3, 1, 0, 0, 0});
            return full_screen_draw_datas;
        };
        Sampler linear_clamp_sampler{SF_LINEAR, SAM_CLAMP_TO_EDGE};
        _cmd_list
            .Gfx(
                tone_mapping_pass_pipeline,
                tone_mapping_constants,
                _src_tex,
                exposure_buffer,
                color_lut,
                linear_clamp_sampler
            )
            .Draw(MOER_TEXT("ToneMapping"),
                Rect2D(0, 0, _target->GetExtent().x, _target->GetExtent().y),
                std::move(get_full_screen_draw_datas()),
                ColorAttachment(_target)
            );
    }
}

void ToneMappingPass::ResetHistogram(CommandList& _cmd_list) {
    _cmd_list.ClearResource(histogram_buffer->GetView(), 0);
}

void ToneMappingPass::ResetExposure(CommandList& _cmd_list) {
    _cmd_list.ClearResource(exposure_buffer->GetView(), 0);
}

void ToneMappingPass::ComputeHistogram(CommandList& _cmd_list, TextureRef _src_tex) {

    _cmd_list.Compute(histogram_pipeline, tone_mapping_constants, _src_tex, histogram_buffer)
        .Dispatch(
            uint3((_src_tex->GetExtent().x + 15) / 16, (_src_tex->GetExtent().y + 15) / 16, 1),
            MOER_TEXT("Calculate Histogram")
        );
}

void ToneMappingPass::ComputeExposure(CommandList& _cmd_list, Params _params) {
    _cmd_list.Compute(exposure_pipeline, tone_mapping_constants, histogram_buffer, exposure_buffer)
        .Dispatch(uint3(1, 1, 1), MOER_TEXT("Calculate Exposure"));
}

void ToneMappingPass::AdvanceFrame(float _elapsed_time) {
    frame_time = _elapsed_time;
    ++frame_idx;
}

} // namespace Moer::Render::Raytracing