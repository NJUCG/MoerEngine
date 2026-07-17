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
    SceneUpdateBatch             scene_updates{};
    RaytracingSceneFrameSnapshot scene_snapshot{};
    bool                         runtime_assets_ready = false;
    RaytracingDebugFrameInput    debug_input{};

    UiCompositionFrameData ui_composition{};
    UiDrawFramePacket      ui_draw_frame{};
};

struct RaytracingFrameFeedback {
    uint64 frame_id = 0;
    PresentReceiptRef main_present_receipt{};

    bool  has_grid_cell_size = false;
    float grid_cell_size     = 1.0f;

    bool export_request_finished = false;
    bool export_consumed         = false;

    ProfileData        profiler_data{};
    Array<std::string> material_texture_names;

    uint64 renderer_tlas_build_count = 0;
    uint64 renderer_tlas_skip_count  = 0;
    uint64 scene_tlas_update_count   = 0;
    uint64 rt_instance_revision      = 0;
    uint64 current_tlas_revision     = 0;
    uint64 previous_tlas_revision    = 0;
    uint configured_local_light_sample_mode    = s_di_local_light_sample_mode_uniform;
    uint effective_local_light_sample_mode     = s_di_local_light_sample_mode_uniform;
    bool adaptive_local_light_fallback_applied = false;
    uint local_light_count                     = 0;
};

} // namespace Moer::Render::Raytracing
