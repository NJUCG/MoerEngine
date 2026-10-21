#pragma once
#include "rhi/RHICommand.h"
#include "scene/gaussian/GaussianRenderSettings.h"
#include <filesystem>
#include <future>
namespace Moer::Render::Raster {
// Read back the image and its capacity flag from the same GPU frame, then encode off-thread.
class RENDER_API GaussianImageExport {
public:
    void Request(
        CommandList&                 commands,
        TextureView                  linear_display,
        BufferView                   frame_status,
        uint2                        resolution,
        const std::filesystem::path& path,
        uint64                       request_id
    );
    void                        Reject(uint64 request_id, std::string message);
    void                        Poll();
    void                        Finish(); // Only for teardown after the renderer has drained GPU submissions.
    const GaussianExportStatus& GetStatus() const {
        return m_status;
    }

private:
    struct WriteResult {
        bool        is_successful;
        std::string message;
    };
    ReadbackFuture           m_image_readback;
    ReadbackFuture           m_status_readback;
    std::future<WriteResult> m_write;
    std::filesystem::path    m_path;
    uint2                    m_resolution{};
    GaussianExportStatus     m_status;
    uint64                   m_active_request_id = 0;
};
} // namespace Moer::Render::Raster
