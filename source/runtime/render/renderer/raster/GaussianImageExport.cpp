#include "GaussianImageExport.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <fstream>
#include <stb/stb_image_write.h>
#include <stdexcept>
#if PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif
namespace Moer::Render::Raster {
namespace {
std::atomic<uint64> g_export_sequence{0};
void WritePng(const std::filesystem::path& path, uint2 resolution, const ReadbackResult& image) {
    const auto   pixels      = image.CopyAs<float>();
    const size_t pixel_count = size_t(resolution.x) * resolution.y;
    if (!pixels || pixels->size() != pixel_count * 4 || !pixel_count)
        throw std::runtime_error("Invalid Gaussian image readback");
    Array<unsigned char> encoded(pixel_count * 3);
    for (size_t pixel_id = 0; pixel_id < pixel_count; ++pixel_id)
        for (size_t channel_id = 0; channel_id < 3; ++channel_id) {
            const float linear = (*pixels)[pixel_id * 4 + channel_id];
            if (!std::isfinite(linear))
                throw std::runtime_error("Gaussian output contains a nonfinite color");
            const float srgb =
                linear <= .0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1.f / 2.4f) - .055f;
            encoded[pixel_id * 3 + channel_id] =
                static_cast<unsigned char>(std::lround(std::clamp(srgb, 0.f, 1.f) * 255.f));
        }
    auto temporary_path = path;
    temporary_path += ".tmp-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                      "-" + std::to_string(g_export_sequence++);
    try {
        {
            std::ofstream output(temporary_path, std::ios::binary | std::ios::trunc);
            if (!output)
                throw std::runtime_error("Cannot open PNG destination");
            const auto callback = [](void* context, void* data, int size) {
                static_cast<std::ofstream*>(context)->write(static_cast<const char*>(data), size);
            };
            if (!stbi_write_png_to_func(
                    callback,
                    &output,
                    int(resolution.x),
                    int(resolution.y),
                    3,
                    encoded.data(),
                    int(resolution.x) * 3
                ))
                throw std::runtime_error("PNG encoder failed");
            output.close();
            if (!output)
                throw std::runtime_error("PNG write failed");
        }
#if PLATFORM_WINDOWS
        if (!::MoveFileExW(
                temporary_path.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
            ))
            throw std::runtime_error("Cannot publish PNG destination");
#else
        std::filesystem::rename(temporary_path, path);
#endif
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary_path, ignored);
        throw;
    }
}
} // namespace
void GaussianImageExport::Reject(uint64 request_id, std::string message) {
    m_status = {request_id, false, false, std::move(message)};
}
void GaussianImageExport::Request(
    CommandList&                 commands,
    TextureView                  display,
    BufferView                   frame_status,
    uint2                        resolution,
    const std::filesystem::path& path,
    uint64                       request_id
) {
    if (m_image_readback.Valid() || m_write.valid()) {
        m_status = {request_id, true, false, "PNG request rejected: previous export is still running"};
        return;
    }
    auto extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) {
        return char(std::tolower(c));
    });
    if (extension != ".png") {
        Reject(request_id, "PNG export requires a .png filename");
        return;
    }
    m_status            = {request_id, true, false, "Waiting for Gaussian GPU image..."};
    m_active_request_id = request_id;
    m_path              = path;
    m_resolution        = resolution;
    m_image_readback    = commands.Readback(display, "Gaussian PNG display");
    m_status_readback   = commands.Readback(frame_status, "Gaussian PNG validity");
    if (!m_image_readback.Valid() || !m_status_readback.Valid()) {
        m_image_readback  = {};
        m_status_readback = {};
        Reject(request_id, "GPU readback was rejected");
    }
}
void GaussianImageExport::Poll() {
    if (m_write.valid() && m_write.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const auto result = m_write.get();
        m_status.is_busy  = false;
        if (m_status.request_id == m_active_request_id) {
            m_status.is_successful = result.is_successful;
            m_status.message       = result.message;
        }
    }
    if (!m_image_readback.Valid() || !m_status_readback.Valid() ||
        m_image_readback.Status() == ReadbackStatus::Pending ||
        m_status_readback.Status() == ReadbackStatus::Pending)
        return;
    const auto image    = m_image_readback.Get();
    const auto status   = m_status_readback.Get();
    m_image_readback    = {};
    m_status_readback   = {};
    const auto overflow = status.ReadValue<uint>(sizeof(uint));
    if (image.status != ReadbackStatus::Ready || !overflow || *overflow != 0) {
        Reject(
            m_status.request_id, "Export failed: GPU readback/capacity error; invalid frame was not written"
        );
        return;
    }
    m_status.message      = "Encoding Gaussian PNG...";
    const auto path       = m_path;
    const auto resolution = m_resolution;
    m_write               = std::async(std::launch::async, [path, resolution, image]() -> WriteResult {
        try {
            WritePng(path, resolution, image);
            return {true, "Gaussian PNG saved (scene only, no UI)"};
        } catch (const std::exception& error) {
            return {false, std::string("PNG export failed: ") + error.what()};
        }
    });
}
void GaussianImageExport::Finish() {
    if (m_image_readback.Valid())
        m_image_readback.Wait();
    if (m_status_readback.Valid())
        m_status_readback.Wait();
    Poll();
    if (m_write.valid()) {
        m_write.wait();
        Poll();
    }
}
} // namespace Moer::Render::Raster
