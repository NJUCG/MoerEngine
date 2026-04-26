#pragma once

#include "Core.h"
#include "renderer/raster/RasterConfig.h"
#include "renderer/common/ui/synapse/Synapse.h"

namespace Moer {

class CooperativeOpsUI {
public:
    explicit CooperativeOpsUI(RasterConfig& config);
    ~CooperativeOpsUI() = default;

    void ShowConfig(Synapse::Context& ui);

private:
    RasterConfig& m_config;
};

} // namespace Moer