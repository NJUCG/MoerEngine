#pragma once

#include "RasterConfig.h"
#include "renderer/EditorConfig.h"
#include "renderer/Renderer.h"
#include "renderer/SceneRenderExtent.h"
#include "scene/Scene.h"
#include "scene/camera/Camera.h"

#include <cstdint>
#include <string>
#include <type_traits>

namespace Moer::Render::Raster {

struct RasterFramePacket {
    uint64_t                   frame_id = 0;
    WindowFrameSnapshot window{};
    RasterConfig               raster_config{};
    std::string                validation_selected_frame_buffer_name;
    EEditorViewportMode        active_viewport_mode = EEditorViewportMode::Game;
    SceneViewGizmoConfig       scene_view_gizmos{};
    Camera                     render_camera{};
    uint2                      camera_viewport_resolution{};
    SceneRenderExtentRequest   scene_render_extent{};
    UiCompositionFrameData     ui_composition{};
    UiDrawFramePacket          ui_draw_frame{};
    SceneUpdateBatch           scene_updates{};
};

static_assert(std::is_move_constructible_v<RasterFramePacket>);
static_assert(!std::is_copy_constructible_v<RasterFramePacket>);

struct RasterFrameFeedback {
    uint64_t                    frame_id = 0;
    PresentReceiptRef           main_present_receipt{};
    RasterConfig::CullingStats  culling_stats{};
    CooperativeOpsStatus        cooperative_ops_status{};
    Array<std::string>          displayable_frame_buffer_names;
};

} // namespace Moer::Render::Raster
