#pragma once

#include "misc/Types.h"

// 通过 ImGui 面板提供 Raytracing 渲染器的运行时配置。

#include "renderer/raytracing/RaytracingConfig.h"
#include "renderer/raytracing/RaytracingDebugState.h"

namespace Moer {

class RaytracingUI {
public:
    RaytracingUI(RaytracingConfig& config, Render::Raytracing::RaytracingDebugState& debug_state);
    ~RaytracingUI() = default;

    void ShowConfig();
    void ShowDebug();

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
    RaytracingConfig&                        m_config;
    UnorderedMap<std::string, uint32>         m_final_color_options;
    Render::Raytracing::RaytracingDebugState& m_debug_state;
};

} // namespace Moer
