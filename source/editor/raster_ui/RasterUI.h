#pragma once

#include "Core.h"
#include "misc/Traits.h"
#include "renderer/raster/RasterConfig.h"
#include "rhi/RHIResource.h"

namespace Moer {

class RasterUI {

public:
    RasterUI(RasterConfig& config);
    ~RasterUI() = default;

    void ShowConfig();

    const RasterConfig& GetConfig() const {
        return m_config;
    }

    Render::TextureView GetSelectedFrameBuffer() const;

    void RegisterFrameBuffers(const Array<Render::TextureView>& frame_buffer_and_name_array);

private:
    uint GetDefaultSelectedFrameBufferIndex() const;

private:
    Array<Render::TextureView> m_frame_buffer_and_name_array;

    RasterConfig& m_config;
};

} // namespace Moer