#pragma once

#include "misc/Traits.h"

#include <string>

namespace Moer {

struct RasterConfig {
    uint aa_mode = 3; // default ssma 1x

    uint  ao_mode           = 1; // default ssao
    float ssao_intensity    = 1.0f;
    int   ssao_sample_count = 8;
    int   ssao_radius       = 2;
    float ssao_max_distance = 0.1f;

    bool  ssr_is_enable_ssr              = true;
    int   ssr_sample_count               = 32;
    bool  ssr_is_enable_jitter           = true;
    bool  ssr_is_force_ground_enable_ssr = true;
    float ssr_roughness_threshold        = 0.5;
    float ssr_metallic_threshold         = 0.5;
    float ssr_step_base                  = 0.025;

    std::string default_selected_frame_buffer_name = "aa_output";
    uint        selected_frame_buffer_index        = 0;
};

} // namespace Moer