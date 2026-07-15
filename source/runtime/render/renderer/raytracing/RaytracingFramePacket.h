#pragma once

#include "RaytracingConfig.h"
#include "RaytracingSceneFrameSnapshot.h"
#include "renderer/Renderer.h"
#include "rhi/RHICommand.h"

namespace Moer::Render::Raytracing {

struct RaytracingDebugFrameInput {
    bool        show_final_texture = false;
    bool        use_bindless       = true;
    int         mip_level          = 0;
    std::string selected_material_texture_name;
};

struct RaytracingFramePacket {
    RaytracingFramePacket() = default;

    RaytracingFramePacket(const RaytracingFramePacket&)            = delete;
    RaytracingFramePacket& operator=(const RaytracingFramePacket&) = delete;
    RaytracingFramePacket(RaytracingFramePacket&&)                 = default;
    RaytracingFramePacket& operator=(RaytracingFramePacket&&)      = default;

    uint64 frame_id = 0;

    Renderer::WindowFrameState   window{};
    RaytracingConfig             config{};
    CameraFrameInput             camera_input{};
    SceneUpdateBatch             scene_updates{};
    RaytracingSceneFrameSnapshot scene_snapshot{};
    bool                         runtime_assets_ready = false;
    RaytracingDebugFrameInput    debug_input{};

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

    ProfileData        profiler_data{};
    Array<std::string> material_texture_names;
};

} // namespace Moer::Render::Raytracing
