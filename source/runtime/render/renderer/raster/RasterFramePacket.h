#pragma once

#include "RasterConfig.h"
#include "renderer/EditorConfig.h"
#include "renderer/Renderer.h"
#include "scene/camera/Camera.h"

#include <cstdint>

namespace Moer::Render::Raster {

struct RasterFramePacket {
    uint64_t                   frame_id = 0;
    Renderer::WindowFrameState window{};
    RasterConfig               raster_config{};
    EEditorViewportMode        active_viewport_mode = EEditorViewportMode::Game;
    SceneViewGizmoConfig       scene_view_gizmos{};
    CameraFrameInput           camera_input{};
    UiCompositionFrameData     ui_composition{};
};

struct RasterFrameFeedback {
    uint64_t                    frame_id = 0;
    RasterConfig::CullingStats  culling_stats{};
    CooperativeOpsStatus        cooperative_ops_status{};
};

} // namespace Moer::Render::Raster
