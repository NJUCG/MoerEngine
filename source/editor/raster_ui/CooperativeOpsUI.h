#pragma once

// 显示 Raster 渲染器的 Cooperative Matrix/Vector 能力与运行状态。

#include "Core.h"
#include "renderer/raster/RasterConfig.h"

namespace Moer {

class CooperativeOpsUI {
public:
    explicit CooperativeOpsUI(RasterConfig& config);
    ~CooperativeOpsUI() = default;

    void ShowConfig();

private:
    RasterConfig& m_config;
};

} // namespace Moer
