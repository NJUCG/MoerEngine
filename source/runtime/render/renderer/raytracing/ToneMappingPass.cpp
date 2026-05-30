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

namespace {

struct RGToneMappingUploadParams {
    DEFINE_RG_BUFFER_ACCESS(constants, EBufferState::TRANSFER_DST);
    DEFINE_RG_PARAMETER_ACCESS(constants);
};

struct RGToneMappingResetExposureParams {
    DEFINE_RG_BUFFER_ACCESS(exposure, EBufferState::TRANSFER_DST);
    DEFINE_RG_PARAMETER_ACCESS(exposure);
};

struct RGToneMappingResetHistogramParams {
    DEFINE_RG_BUFFER_ACCESS(histogram, EBufferState::TRANSFER_DST);
    DEFINE_RG_PARAMETER_ACCESS(histogram);
};

struct RGToneMappingHistogramParams {
    DEFINE_RG_BUFFER_ACCESS(constants, EBufferState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(source, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_BUFFER_ACCESS(histogram, EBufferState::UNORDERED_ACCESS);
    DEFINE_RG_PARAMETER_ACCESS(constants, source, histogram);
};

struct RGToneMappingExposureParams {
    DEFINE_RG_BUFFER_ACCESS(constants, EBufferState::SHADER_RESOURCE);
    DEFINE_RG_BUFFER_ACCESS(histogram, EBufferState::SHADER_RESOURCE);
    DEFINE_RG_BUFFER_ACCESS(exposure, EBufferState::UNORDERED_ACCESS);
    DEFINE_RG_PARAMETER_ACCESS(constants, histogram, exposure);
};

struct RGToneMappingRenderParams {
    DEFINE_RG_BUFFER_ACCESS(constants, EBufferState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(source, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_BUFFER_ACCESS(exposure, EBufferState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(color_lut, ETextureState::SAMPLED);
    DEFINE_RG_TEXTURE_ACCESS(target, ETextureState::RENDER_TARGET);
    DEFINE_RG_PARAMETER_ACCESS(constants, source, exposure, color_lut, target);
};

} // namespace

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
        if (color_lut->GetHeight() * color_lut->GetHeight() == color_lut->GetWidth()) {
            color_lut_size = color_lut->GetHeight();
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

ToneMappingPass::PreparedCommand ToneMappingPass::Prepare(Params _params, TextureRef _src_tex) {
    bool b_enable_lut = _params.enable_color_lut && color_lut_size > 0;

    PreparedCommand command{};
    command.params.log_luminance_scale          = 1.f / (g_max_log_luminamce - g_min_log_luminance);
    command.params.log_luminance_bias           = -g_min_log_luminance * command.params.log_luminance_scale;
    command.params.log_luminance_scale_exposure = g_max_log_luminamce - g_min_log_luminance;
    command.params.log_luminance_bias_exposure  = g_min_log_luminance;
    command.params.histogram_low_percentile     = std::min(0.99f, std::max(0.f, _params.histogram_low_percentile));
    command.params.histogram_high_percentile =
        std::min(1.f, std::max(_params.histogram_high_percentile, _params.histogram_low_percentile));
    command.params.eye_adaptation_speed_up   = _params.eye_adaptation_speed_up;
    command.params.eye_adaptation_speed_down = _params.eye_adaptation_speed_down;
    command.params.min_adapted_luminance     = _params.min_adapted_luminance;
    command.params.max_adapted_luminance     = _params.max_adapted_luminance;
    command.params.frame_time                = frame_time;
    command.params.view_origin               = uint2(0, 0);
    command.params.view_size                 = uint2(_src_tex->GetExtent().x, _src_tex->GetExtent().y);
    command.params.exposure_scale            = std::exp2f(_params.exposure_bias);
    command.params.white_point_inv_squared   = 1.f / (_params.white_point * _params.white_point);
    command.params.source_slice              = 0;
    command.params.color_lut_size =
        b_enable_lut ? float2(color_lut_size * color_lut_size, color_lut_size) : float2(0.f);
    command.params.color_lut_size_inv =
        b_enable_lut ? float2(1.f / (color_lut_size * color_lut_size), 1.f / color_lut_size) : float2(0.f);
    command.params.frame_idx = frame_idx;
    command.params.enabled   = _params.enable_tone_mapping ? 1 : 0;

    if (!b_enabled != _params.enable_tone_mapping) {
        b_enabled              = _params.enable_tone_mapping;
        command.reset_exposure = true;
    }

    return command;
}

ToneMappingPass::RecordResources ToneMappingPass::CaptureResources(TextureRef _src_tex, TextureRef _target) const {
    return RecordResources{.src_tex = _src_tex, .target = _target, .color_lut = color_lut};
}

void ToneMappingPass::RecordConstantsUpload(CommandList& _cmd_list, const PreparedCommand& _command) {
    Array<byte> upload_data(sizeof(ToneMappingParams));
    upload_data.assign((byte*)&_command.params, (byte*)&_command.params + sizeof(ToneMappingParams));

    _cmd_list.CopyFrom(std::move(upload_data), tone_mapping_constants->GetView());
}

void ToneMappingPass::Render(
    CommandList& _cmd_list,
    TextureRef   _src_tex,
    TextureRef   _target,
    TextureRef   _color_lut
) {
    TextureRef lut = _color_lut ? _color_lut : _src_tex;

    bool draw_indirect = false;
    Array<byte> upload_data;

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
                lut,
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
                lut,
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

void ToneMappingPass::ComputeExposure(CommandList& _cmd_list) {
    _cmd_list.Compute(exposure_pipeline, tone_mapping_constants, histogram_buffer, exposure_buffer)
        .Dispatch(uint3(1, 1, 1), MOER_TEXT("Calculate Exposure"));
}

void ToneMappingPass::AddPasses(
    RenderGraph&                 _graph,
    const RTGraphFrameResources& _rg,
    const RTContext&             _rt_ctx,
    Params                       _params,
    TextureRef                   _src_tex,
    TextureRef                   _target
) {
    auto* command   = _graph.Alloc<PreparedCommand>(Prepare(_params, _src_tex));
    auto* resources = _graph.Alloc<RecordResources>(CaptureResources(_src_tex, _target));

    RGBuffer* constants = _graph.ImportBuffer(
        MOER_TEXT("RT.ToneMapping.constants"), tone_mapping_constants, EQueueType::Graphics
    );
    RGBuffer* histogram = _graph.ImportBuffer(
        MOER_TEXT("RT.ToneMapping.histogram"), histogram_buffer, EQueueType::Graphics
    );
    RGBuffer* exposure = _graph.ImportBuffer(MOER_TEXT("RT.ToneMapping.exposure"), exposure_buffer, EQueueType::Graphics);
    RGTexture* source  = RTGraphTextureForFrameTexture(_rg, _rt_ctx, _src_tex);
    RGTexture* target  = RTGraphTextureForFrameTexture(_rg, _rt_ctx, _target);
    RGTexture* lut     = ImportRTTextureIfValid(_graph, MOER_TEXT("RT.ToneMapping.color_lut"), color_lut);

    auto* upload_params      = _graph.Alloc<RGToneMappingUploadParams>();
    upload_params->constants = RGBufferView{.buffer = constants};
    _graph.AddPass(
        MOER_TEXT("RT.ToneMapping.UploadConstants"),
        upload_params,
        ERGPassFlags::Graphics,
        [this, command](RHICommandList& cmd_list, RGContext) {
            RecordConstantsUpload(cmd_list, *command);
        }
    );

    if (command->reset_exposure) {
        auto* reset_exposure_params     = _graph.Alloc<RGToneMappingResetExposureParams>();
        reset_exposure_params->exposure = RGBufferView{.buffer = exposure};
        _graph.AddPass(
            MOER_TEXT("RT.ToneMapping.ResetExposure"),
            reset_exposure_params,
            ERGPassFlags::Graphics,
            [this](RHICommandList& cmd_list, RGContext) {
                ResetExposure(cmd_list);
            }
        );
    }

    auto* reset_histogram_params      = _graph.Alloc<RGToneMappingResetHistogramParams>();
    reset_histogram_params->histogram = RGBufferView{.buffer = histogram};
    _graph.AddPass(
        MOER_TEXT("RT.ToneMapping.ResetHistogram"),
        reset_histogram_params,
        ERGPassFlags::Graphics,
        [this](RHICommandList& cmd_list, RGContext) {
            ResetHistogram(cmd_list);
        }
    );

    auto* histogram_params      = _graph.Alloc<RGToneMappingHistogramParams>();
    histogram_params->constants = RGBufferView{.buffer = constants};
    histogram_params->source    = RTWholeTextureView(source);
    histogram_params->histogram = RGBufferView{.buffer = histogram};
    _graph.AddPass(
        MOER_TEXT("RT.ToneMapping.Histogram"),
        histogram_params,
        kRTGraphComputePassFlags,
        [this, resources](RHICommandList& cmd_list, RGContext) {
            ComputeHistogram(cmd_list, resources->src_tex);
        }
    );

    auto* exposure_params      = _graph.Alloc<RGToneMappingExposureParams>();
    exposure_params->constants = RGBufferView{.buffer = constants};
    exposure_params->histogram = RGBufferView{.buffer = histogram};
    exposure_params->exposure  = RGBufferView{.buffer = exposure};
    _graph.AddPass(
        MOER_TEXT("RT.ToneMapping.Exposure"),
        exposure_params,
        kRTGraphComputePassFlags,
        [this](RHICommandList& cmd_list, RGContext) {
            ComputeExposure(cmd_list);
        }
    );

    auto* render_params      = _graph.Alloc<RGToneMappingRenderParams>();
    render_params->constants = RGBufferView{.buffer = constants};
    render_params->source    = RTWholeTextureView(source);
    render_params->exposure  = RGBufferView{.buffer = exposure};
    render_params->color_lut = RTWholeTextureView(lut);
    render_params->target    = RTWholeTextureView(target);
    _graph.AddPass(
        MOER_TEXT("RT.ToneMapping.Render"),
        render_params,
        ERGPassFlags::Graphics,
        [this, resources](RHICommandList& cmd_list, RGContext) {
            Render(cmd_list, resources->src_tex, resources->target, resources->color_lut);
        }
    );
}

void ToneMappingPass::AdvanceFrame(float _elapsed_time) {
    frame_time = _elapsed_time;
    ++frame_idx;
}

} // namespace Moer::Render::Raytracing