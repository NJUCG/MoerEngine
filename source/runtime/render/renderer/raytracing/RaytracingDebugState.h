#pragma once

#include "rhi/RHIProfileData.h"

#include <string>

namespace Moer::Render::Raytracing {

// Copied into the frame packet; render work never reads mutable editor state.
struct RaytracingDebugFrameInput {
    bool        show_final_texture = false;
    bool        use_bindless       = true;
    int         mip_level          = 0;
    std::string selected_material_texture_name;
};

// Game Thread state: UI edits input and consumes results from existing frame feedback.
struct RaytracingDebugState {
    RaytracingDebugFrameInput input{};
    ProfileData              profiler_data{};
    Array<std::string>       material_texture_names;
};

} // namespace Moer::Render::Raytracing
