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
    EGaussianSceneContent      gaussian_content = EGaussianSceneContent::Empty;
    bool                       has_gaussian_assets = false;
    GaussianViewSettings       gaussian_view_settings{};
    std::optional<GaussianExportRequest> gaussian_export_request;
};

static_assert(std::is_move_constructible_v<RasterFramePacket>);
static_assert(!std::is_copy_constructible_v<RasterFramePacket>);

struct RasterFrameFeedback {
    GaussianRasterStatus        gaussian_status{};
    GaussianExportStatus        gaussian_export_status{};
    uint64_t                    frame_id = 0;
    PresentReceiptRef           main_present_receipt{};
    RasterConfig::CullingStats  culling_stats{};
    CooperativeOpsStatus        cooperative_ops_status{};
    Array<std::string>          displayable_frame_buffer_names;
};

} // namespace Moer::Render::Raster
