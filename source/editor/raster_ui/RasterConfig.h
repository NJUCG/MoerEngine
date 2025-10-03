#pragma once

#include "misc/STL.h"
#include "misc/Traits.h"

#include "raster/RasterCompileTimeConstants.h"

#include <string>

namespace Moer {

enum class EShadingMode { DEFAULT = 0, DEBUG, NUM };
static const Array<std::string> s_shading_mode_name_array = {"Default", "Debug"};

enum class EAaMode { NONE = 0, FXAA_SIMPLIFIED, FXAA_QUALITY, SMAA_1X, SMAA_T2X, NUM };
static const Array<std::string> s_aa_mode_name_array = {
    "None",
    "FXAA Simplified",
    "FXAA Quality",
    "SMAA 1x",
    "SMAA T2x",
};

enum class EAoMode { NONE = 0, SSAO, SSAO_AO_ONLY, RTAO, RTAO_AO_ONLY, LINEARIZED_DEPTH_DIV_10, NUM };
static const Array<std::string> s_ao_mode_name_array = {
    "None",
    "SSAO",
    "SSAO AO Only",
    "RTAO",
    "RTAO AO Only",
    "Linear. Depth / 10.0",
};

enum class ERtaoSampleMode { UNIFORM = 0, COSINE_WEIGHTED, NUM };
static const Array<std::string> s_rtao_sample_mode = {
    "Uniform in Semisphere",
    "Cosine-Weighted in Semisphere"
};

static const Array<std::string> s_shadow_map_mode_name_array = {
    "Disabled",
    "Cascaded SM",
    "Virtual SM",
};
static const Array<std::string> s_shadow_sampling_mode_name_array = {
    "No Filtering",
    "PCF 1x1",
    "PCF 3x3",
    "PCF 5x5",
};

struct RasterConfig {

    // MARK: Shading
    EShadingMode shading_mode                    = EShadingMode::DEFAULT;
    bool         shading_enable_extra_ambient    = true;
    float3       shading_extra_ambient_color     = float3(1.f, 1.f, 1.f);
    float        shading_extra_ambient_intensity = 0.1f;

    // MARK: AA
    EAaMode aa_mode = EAaMode::SMAA_1X;

    // MARK: AO
    EAoMode         ao_mode                 = EAoMode::SSAO;
    float           ssao_intensity          = 1.0f;
    int             ssao_spp                = 8;
    int             ssao_sample_radius      = 2;
    float           ssao_max_distance       = 0.5f;
    ERtaoSampleMode rtao_sample_mode        = ERtaoSampleMode::COSINE_WEIGHTED;
    float           rtao_intensity          = 1.0f;
    float           rtao_ray_trace_distance = 1.0f;
    int             rtao_spp                = 1;

    // MARK: SSR
    bool  ssr_is_ssr_enabled             = false;
    int   ssr_sample_count               = 32;
    bool  ssr_is_enable_jitter           = true;
    bool  ssr_is_force_ground_enable_ssr = true;
    float ssr_roughness_threshold        = 0.5;
    float ssr_metallic_threshold         = 0.5;
    float ssr_step_base                  = 0.025;

    // MARK: AI (CUDA, TensorRT)
    bool ai_is_cuda_enabled = false;

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