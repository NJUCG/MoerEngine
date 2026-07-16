#pragma once

// Presents raster renderer settings and diagnostic output selection in the editor.

#include "CooperativeOpsUI.h"

#include "Core.h"
#include "misc/Traits.h"
#include "renderer/raster/RasterConfig.h"
#include "rhi/RHIResource.h"

namespace Moer {

class RasterUI {

public:
    explicit RasterUI(RasterConfig& config);
    ~RasterUI() = default;

    void ShowConfig();

    const RasterConfig& GetConfig() const {
        return m_config;
    }

    void RegisterFrameBufferNames(const Array<std::string>& frame_buffer_names);

private:
    uint GetDefaultSelectedFrameBufferIndex() const;

private:
    Array<std::string> m_frame_buffer_names;
    bool               m_frame_buffer_names_initialized = false;

    CooperativeOpsUI m_cooperative_ops_ui;
    RasterConfig&     m_config;
};

} // namespace Moer
