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
    ExportSubmissionTransaction& export_submission
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

    size_t            size = 0;
    Array<Moer::byte> copy_back_data;
    std::string       file_name = "screenshot_";
    bool              hdr       = false;

    const uint3 resolution = frame_resources.ldr_color->GetExtent();
    switch (config.output_texture) {
        case EOT_LDR:
            size = sizeof(uint) * resolution.x * resolution.y;
            copy_back_data.resize(size);
            command_list.CopyFrom(frame_resources.ldr_color->GetView(), copy_back_data);
            file_name += suffix;
            file_name += ".png";
            break;
        case EOT_HDR:
            size = sizeof(float2) * resolution.x * resolution.y;
            copy_back_data.resize(size);
            command_list.CopyFrom(frame_resources.resolved_color->GetView(), copy_back_data);
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

    export_submission.BeginReadback(device.CreateFence());
    CmdSubmit readback_submit =
        command_list.Submit().TickProfiling();
    export_submission.AttachReadbackSignal(readback_submit);
    RHIExecutor::Get().Submit(
        EQueueType::Graphics,
        std::move(readback_submit),
        ERHIExecSubmitFlags::FlushGPU
    );
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    if (!export_submission.ResolveReadbackAcceptance()) {
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

    return export_submission.DispatchEncoder([&] {
        LambdaTask::Create([=,
                            data(std::move(copy_back_data)),
                            dequantize_byte_to_srgb(std::move(dequantize_byte_to_srgb)),
                            dequantize_half(std::move(dequantize_half))]() mutable {
            auto copy_back_data = std::move(data);
            if (hdr) {
                constexpr uint range_count = 8;
                const uint     range       = copy_back_data.size() / range_count;
                Array<float4>  copy_back_data_f4(copy_back_data.size() / 8);
                ParallelFor(range_count, [&](uint index) {
                    const size_t start = range * index;
                    const size_t end   = range * (index + 1);
                    for (size_t i = start; i < copy_back_data.size() && i < end; i += 8) {
                        float4* output = &copy_back_data_f4[i / 8];
                        short*  input  = reinterpret_cast<short*>(&copy_back_data[i]);
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
                const uint     range       = copy_back_data.size() / range_count;
                ParallelFor(range_count, [&](uint index) {
                    const size_t start = range * index;
                    const size_t end   = range * (index + 1);
                    for (size_t i = start; i < copy_back_data.size() && i < end; i += 4) {
                        copy_back_data[i] =
                            static_cast<Moer::byte>(dequantize_byte_to_srgb(ubyte(copy_back_data[i])));
                        copy_back_data[i + 1] =
                            static_cast<Moer::byte>(dequantize_byte_to_srgb(ubyte(copy_back_data[i + 1])));
                        copy_back_data[i + 2] =
                            static_cast<Moer::byte>(dequantize_byte_to_srgb(ubyte(copy_back_data[i + 2])));
                        copy_back_data[i + 3] =
                            static_cast<Moer::byte>(dequantize_byte_to_srgb(ubyte(copy_back_data[i + 3])));
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
