#pragma once

// Exposes the ray-tracing renderer's runtime configuration through an ImGui panel.

#include "renderer/raytracing/RaytracingConfig.h"

namespace Moer {

class RaytracingUI {
public:
    explicit RaytracingUI(RaytracingConfig& config);
    ~RaytracingUI() = default;

    void ShowConfig();

    const RaytracingConfig& GetConfig() const {
        return m_config;
    }

    RaytracingConfig& GetEditableConfig() {
        return m_config;
    }

    void ResetConfig() {
        m_config = {};
    }

private:
    RaytracingConfig&               m_config;
    UnorderedMap<std::string, uint> m_final_color_options;
};

} // namespace Moer
