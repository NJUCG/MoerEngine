#pragma once

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