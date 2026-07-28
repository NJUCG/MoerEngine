#include "RaytracingRenderer.h"

#include "RTResource.h"
#include "RaytracingExportSubmission.h"
#include "rhi/RHIExecutor.h"
#include "taskgraph/TaskGraph.h"

#include <stb/stb_image_write.h>

#include <atomic>
#include <cmath>

namespace Moer::Render::Raytracing {

namespace {

union FloatBits {
    float        f;
    unsigned int ui;
};

} // namespace

bool RaytracingRenderer::DumpTextureToFile(
    const ExportConfig&    config,
    FrameResources&        frame_resources,
    RenderDevice&          device,
    CommandQueue&          gfx_queue,
    std::filesystem::path& exported_file_path,
    std::string_view       suffix,
    ExportSubmissionTransaction& export_submission,
    const std::function<bool(CommandList&)>& setup_command_list
) {
    (void)gfx_queue;
    CommandList command_list{};

    auto dequantize_half = [](short value, bool gamma_correct = true) {
        const unsigned int sign = unsigned(value & 0x8000) << 16;
        const int          em   = value & 0x7fff;

        int result = (em + (112 << 10)) << 13;
        result     = (em < (1 << 10)) ? 0 : result;
        result += (em >= (31 << 10)) ? (112 << 23) : 0;

        FloatBits bits;
        bits.ui = sign | result;
        if (gamma_correct) {
            bits.f = bits.f <= 0.0031308f ? 12.92f * bits.f : 1.055f * std::pow(bits.f, 1.f / 2.4f) - 0.055f;
        }
        return bits.f;
    };

    auto dequantize_byte_to_srgb = [](unsigned char value) {
        float color = value / 255.f;
        color       = color <= 0.0031308f ? 12.92f * color : 1.055f * std::pow(color, 1.f / 2.4f) - 0.055f;
        return static_cast<unsigned char>(color * 255.f);
    };

    size_t         size = 0;
    ReadbackFuture readback{};
    std::string    file_name = "screenshot_";
    bool           hdr       = false;

    const uint3 resolution = frame_resources.ldr_color->GetExtent();
    switch (config.output_texture) {
        case EOT_LDR:
            size = sizeof(uint) * resolution.x * resolution.y;
            file_name += suffix;
            file_name += ".png";
            break;
        case EOT_HDR:
            size = sizeof(float2) * resolution.x * resolution.y;
            file_name += suffix;
            file_name += ".exr";
            hdr = true;
            break;
        default:
            break;
    }

    if (size == 0) {
        return false;
    }

    const bool profile_source_bound =
        setup_command_list && setup_command_list(command_list);
    {
        ScopedGpuMarker readback_marker(
            command_list,
            "Raytracing Export Readback",
            GpuMarkerPalette::Transfer(),
            profile_source_bound ?
                EGpuMarkerMode::Timestamp :
                EGpuMarkerMode::Label
        );
        if (config.output_texture == EOT_LDR) {
            readback = command_list.Readback(
                frame_resources.ldr_color->GetView(),
                "RaytracingExportLdrReadback"
            );
        } else {
            readback = command_list.Readback(
                frame_resources.resolved_color->GetView(),
                "RaytracingExportHdrReadback"
            );
        }
    }
    if (!readback.Valid()) {
        return false;
    }

    const bool modern_profiling =
        command_list.HasGpuScopeRecorder() ||
        command_list.IsLegacyGpuProfilingSuppressedForGeneration();
    export_submission.BeginReadback(device.CreateFence());
    CmdSubmit readback_submit = command_list.Submit();
    if (!modern_profiling) {
        readback_submit.TickProfiling();
    }
    export_submission.AttachReadbackSignal(readback_submit);
    RHIExecutor::Get().Submit(
        EQueueType::Graphics,
        std::move(readback_submit),
        ERHIExecSubmitFlags::FlushGPU
    );
    if (!export_submission.ResolveReadbackAcceptance()) {
        return false;
    }
    ReadbackResult readback_result = readback.Get();
    if (readback_result.status != ReadbackStatus::Ready ||
        readback_result.ByteSize() != size) {
        export_submission.MarkAcceptedReadbackFailed();
        LOG_ERROR(
            "Raytracing export readback failed: status={} expected_bytes={} "
            "actual_bytes={} reason={}",
            static_cast<uint32>(readback_result.status),
            size,
            readback_result.ByteSize(),
            readback_result.error_reason
        );
        return false;
    }

    if (hdr) {
        assert(
            frame_resources.resolved_color->GetFormat() == PF_R16G16B16A16_SFLOAT &&
            "resolved color format must be r16g16b16a16_sfloat"
        );
    } else {
        assert(
            frame_resources.ldr_color->GetFormat() == PF_R8G8B8A8_UNORM && "ldr format must be r8g8b8a8_unorm"
        );
    }

    const std::shared_ptr<const Array<byte>> payload =
        readback_result.data;
    return export_submission.DispatchEncoder([&, payload] {
        LambdaTask::Create([=,
                            dequantize_byte_to_srgb(std::move(dequantize_byte_to_srgb)),
                            dequantize_half(std::move(dequantize_half))]() mutable {
            Array<byte> copy_back_data(
                payload->begin(), payload->end()
            );
            const size_t pixel_count =
                static_cast<size_t>(resolution.x) *
                static_cast<size_t>(resolution.y);
            if (hdr) {
                constexpr uint range_count = 8;
                Array<float4>  copy_back_data_f4(pixel_count);
                ParallelFor(range_count, [&](uint index) {
                    const size_t begin =
                        pixel_count * index / range_count;
                    const size_t end =
                        pixel_count * (index + 1) / range_count;
                    for (size_t pixel = begin; pixel < end; ++pixel) {
                        float4* output = &copy_back_data_f4[pixel];
                        short* input = reinterpret_cast<short*>(
                            &copy_back_data[pixel * 8]
                        );
                        output->x      = dequantize_half(input[0]);
                        output->y      = dequantize_half(input[1]);
                        output->z      = dequantize_half(input[2]);
                        output->w      = dequantize_half(input[3]);
                    }
                });
                stbi_write_hdr(
                    (exported_file_path / file_name).generic_string().data(),
                    resolution.x,
                    resolution.y,
                    4,
                    reinterpret_cast<float*>(copy_back_data_f4.data())
                );
            } else {
                constexpr uint range_count = 8;
                ParallelFor(range_count, [&](uint index) {
                    const size_t begin =
                        pixel_count * index / range_count;
                    const size_t end =
                        pixel_count * (index + 1) / range_count;
                    for (size_t pixel = begin; pixel < end; ++pixel) {
                        const size_t offset = pixel * 4;
                        copy_back_data[offset] =
                            static_cast<Moer::byte>(dequantize_byte_to_srgb(ubyte(copy_back_data[offset])));
                        copy_back_data[offset + 1] =
                            static_cast<Moer::byte>(dequantize_byte_to_srgb(ubyte(copy_back_data[offset + 1])));
                        copy_back_data[offset + 2] =
                            static_cast<Moer::byte>(dequantize_byte_to_srgb(ubyte(copy_back_data[offset + 2])));
                        copy_back_data[offset + 3] =
                            static_cast<Moer::byte>(dequantize_byte_to_srgb(ubyte(copy_back_data[offset + 3])));
                    }
                });
                stbi_write_png(
                    (exported_file_path / file_name).generic_string().data(),
                    resolution.x,
                    resolution.y,
                    4,
                    copy_back_data.data(),
                    4 * resolution.x
                );
            }
            std::atomic_thread_fence(std::memory_order_seq_cst);
        }).Dispatch();
    });
}

} // namespace Moer::Render::Raytracing
