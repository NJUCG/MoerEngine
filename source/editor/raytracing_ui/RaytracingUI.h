#pragma once

// 通过 ImGui 面板提供 Raytracing 渲染器的运行时配置。

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
