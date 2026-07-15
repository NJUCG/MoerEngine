#pragma once

#include "RaytracingConfig.h"
#include "renderer/Renderer.h"

namespace Moer::Render::Raytracing {

struct RaytracingFramePacket {
    RaytracingFramePacket() = default;

    RaytracingFramePacket(const RaytracingFramePacket&)            = delete;
    RaytracingFramePacket& operator=(const RaytracingFramePacket&) = delete;
    RaytracingFramePacket(RaytracingFramePacket&&)                 = default;
    RaytracingFramePacket& operator=(RaytracingFramePacket&&)      = default;

    uint64 frame_id = 0;

    Renderer::WindowFrameState window{};
    RaytracingConfig           config{};
    CameraFrameInput           camera_input{};
    SceneUpdateBatch           scene_updates{};
    bool                       runtime_assets_ready = false;

    UiCompositionFrameData ui_composition{};
    UiDrawFramePacket      ui_draw_frame{};
};

struct RaytracingFrameFeedback {
    uint64 frame_id = 0;

    bool   has_main_camera = false;
    Camera main_camera{};

    bool  has_grid_cell_size = false;
    float grid_cell_size     = 1.0f;

    bool export_consumed = false;
};

} // namespace Moer::Render::Raytracing
