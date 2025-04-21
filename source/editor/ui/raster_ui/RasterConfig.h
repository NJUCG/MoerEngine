#pragma once

#include "misc/STL.h"
#include "misc/Traits.h"

#include "raster/RasterCompileTimeConstants.h"

#include <string>

namespace Moer {

struct RasterConfig {
    // MARK: AA
    uint aa_mode = 3; // default ssma 1x

    // MARK: AO
    uint  ao_mode           = 1; // default ssao
    float ssao_intensity    = 1.0f;
    int   ssao_sample_count = 8;
    int   ssao_radius       = 2;
    float ssao_max_distance = 0.1f;

    // MARK: SSR
    bool  ssr_is_enable_ssr              = true;
    int   ssr_sample_count               = 32;
    bool  ssr_is_enable_jitter           = true;
    bool  ssr_is_force_ground_enable_ssr = true;
    float ssr_roughness_threshold        = 0.5;
    float ssr_metallic_threshold         = 0.5;
    float ssr_step_base                  = 0.025;

    // MARK: Shadow
    int shadow_map_mode            = 1; // 0: disabled, 1: CSM, 2: VSM
    int shadow_sampling_mode       = 0;
    int shadow_csm_num_of_cascades = 2;
    int shadow_csm_sm_size         = 2048;

    StaticArray<float, CSM_MAX_CASCADES> shadow_csm_cover_ratio_of_camera = {0.01, 0.04, 0.32, 1.0};

    // MARK: Others
    std::string default_selected_frame_buffer_name = "aa_output";
    uint        selected_frame_buffer_index        = 0;
};

} // namespace Moer