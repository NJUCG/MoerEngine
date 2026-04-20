#ifndef MOER_TEST_RaytracingUI_H
#define MOER_TEST_RaytracingUI_H

#include "Core.h"
#include "renderer/raytracing/RaytracingConfig.h"

namespace Moer {

class RaytracingUI {
public:
    RaytracingUI(RaytracingConfig& config);
    ~RaytracingUI() = default;

    void ShowConfig();

    const RaytracingConfig& GetConfig() const {
        return config;
    }

    RaytracingConfig& GetEditableConfig() {
        return config;
    }

    void ResetConfig() {
        config = RaytracingConfig();
    }

private:
    RaytracingConfig& config;

    UnorderedMap<std::string, uint> final_color_map;
};

} // namespace Moer
#endif