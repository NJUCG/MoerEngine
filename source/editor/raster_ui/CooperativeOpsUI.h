#pragma once

// 显示 Raster 渲染器的 Cooperative Matrix/Vector 能力与运行状态。

#include "Core.h"
#include "renderer/raster/RasterConfig.h"
#include "renderer/raster/RasterFrameStatus.h"

namespace Moer {

class CooperativeOpsUI {
public:
    CooperativeOpsUI(RasterConfig& config, const CooperativeOpsStatus& status);
    ~CooperativeOpsUI() = default;

    void ShowConfig();

private:
    RasterConfig&               m_config;
    const CooperativeOpsStatus& m_status;
};

} // namespace Moer
