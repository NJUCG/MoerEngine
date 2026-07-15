#pragma once

#include "RasterConfig.h"
#include "renderer/EditorConfig.h"
#include "renderer/Renderer.h"
#include "scene/Scene.h"
#include "scene/camera/Camera.h"

#include <cstdint>
#include <type_traits>

namespace Moer::Render::Raster {

struct RasterFramePacket {
    uint64_t                   frame_id = 0;
    Renderer::WindowFrameState window{};
    RasterConfig               raster_config{};
    EEditorViewportMode        active_viewport_mode = EEditorViewportMode::Game;
    SceneViewGizmoConfig       scene_view_gizmos{};
    CameraFrameInput           camera_input{};
    UiCompositionFrameData     ui_composition{};
    UiDrawFramePacket          ui_draw_frame{};
    SceneUpdateBatch           scene_updates{};
};

static_assert(std::is_move_constructible_v<RasterFramePacket>);
static_assert(!std::is_copy_constructible_v<RasterFramePacket>);

struct RasterFrameFeedback {
    uint64_t                    frame_id = 0;
    RasterConfig::CullingStats  culling_stats{};
    CooperativeOpsStatus        cooperative_ops_status{};
    bool                        has_main_camera = false;
    Camera                      main_camera{};
};

} // namespace Moer::Render::Raster
