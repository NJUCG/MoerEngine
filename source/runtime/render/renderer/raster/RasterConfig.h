#pragma once

#include "misc/STL.h"
#include "misc/Traits.h"

#include "RasterCompileTimeConstants.h"
#include "shaderheaders/shared/raster/geometry_pass/ShaderParameters.h"
#include "shaderheaders/shared/raster/lighting_pass/ShaderParameters.h"
#include "shaderheaders/shared/raster/post_process/ShaderParameters.h"

#include <string>

namespace Moer {

enum class EShadingMode {
    DEFAULT = 0,
    DEBUG,
    NUM
};
static const Array<std::string> s_shading_mode_name_array = {"Default", "Debug"};

// Enum 在 ..../ShaderParameters.h中定义，让shader和cpp可以共用枚举值
// EnumParam(EAaMode, NONE, FXAA_SIMPLIFIED, FXAA_QUALITY, SMAA_1X, SMAA_T2X);
static const UnorderedMap<EAaMode, std::string> s_aa_mode_name_map = {
    {EAaMode::NONE, "None"},
    {EAaMode::FXAA_SIMPLIFIED, "FXAA Simplified"},
    {EAaMode::FXAA_QUALITY, "FXAA Quality"},
    {EAaMode::SMAA_1X, "SMAA 1x"},
    {EAaMode::SMAA_T2X, "SMAA T2x"},
};

// EnumParam(EAoMode, NONE, SSAO, SSAO_AO_ONLY, RTAO, RTAO_AO_ONLY, LINEARIZED_DEPTH_DIV_10);
static const UnorderedMap<EAoMode, std::string> s_ao_mode_name_map = {
    {EAoMode::NONE, "None"},
    {EAoMode::SSAO, "SSAO"},
    {EAoMode::SSAO_AO_ONLY, "SSAO AO Only"},
    {EAoMode::RTAO, "RTAO"},
    {EAoMode::RTAO_AO_ONLY, "RTAO AO Only"},
    {EAoMode::SSDO, "SSDO"},
    {EAoMode::SSDO_AO_ONLY, "SSDO AO Only"},
    {EAoMode::LINEARIZED_DEPTH_DIV_10, "Linear. Depth / 10.0"},
};

// EnumParam(EDenoiserMode, NONE, BILATERAL_FILTER);
static const UnorderedMap<EDenoiserMode, std::string> s_denoiser_mode_name_map = {
    {EDenoiserMode::NONE, "None"},
    {EDenoiserMode::BILATERAL_FILTER, "双边滤波 Bilateral Filter"},
};

// EnumParam(ERtaoSampleMode, UNIFORM, COSINE_WEIGHTED);
static const UnorderedMap<ERtaoSampleMode, std::string> s_rtao_sample_mode_name_map = {
    {ERtaoSampleMode::UNIFORM, "Uniform in Semisphere"},
    {ERtaoSampleMode::COSINE_WEIGHTED, "Cosine-Weighted in Semisphere"},
};

enum class EUpsampleMode {
    None = 0,
    BILINEAR,
    DEPTH
};
static const Array<std::string> s_upsample_mode_name_array = {
    "None",
    "BILINEAR",
    "DEPTH",
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
static const Array<std::string> s_ai_trt_visualize_buffer_array = {
    "Engine1 in_depth",           "Engine1 in_color",        "Engine1 in_motion",
    "Engine1 in_prev_ao",         "Engine1 in_prev_embed",   "Engine1 out_XTX_batch",
    "Engine1 out_XTY_batch",      "Engine1 out_X_model",     "Engine1 out_ao",
    "Engine1 out_upscale_kernel", "Engine1 out_color",       "Engine1 out_prev_ao",
    "Engine1 out_embed",          "Engine2 in_X_model",      "Engine2 in_coeffs_batch",
    "Engine2 in_upscale_kernel",  "Engine2 in_color",        "Engine2 in_prev_ao",
    "Engine2 out_final_output",   "Engine2 out_denoised_ao",
}; // namespace Moer

struct RasterConfig {

    // MARK: Shading
    EShadingMode shading_mode                    = EShadingMode::DEFAULT;
    bool         shading_enable_extra_ambient    = true;
    float3       shading_extra_ambient_color     = float3(1.f, 1.f, 1.f);
    float        shading_extra_ambient_intensity = 0.1f;

    // MARK: AA
    EAaMode aa_mode = EAaMode::SMAA_1X;

    // MARK: AO
    EAoMode         ao_mode                     = EAoMode::SSAO;
    float           ssao_intensity              = 1.0f;
    int             ssao_spp                    = 16;
    int             ssao_sample_radius          = 2;
    float           ssao_max_distance           = 0.5f;
    ERtaoSampleMode rtao_sample_mode            = ERtaoSampleMode::COSINE_WEIGHTED;
    float           rtao_intensity              = 1.0f;
    float           rtao_ray_trace_distance     = 1.0f;
    int             rtao_spp                    = 1;
    bool            rtao_denoiser_enable        = true;
    float           rtao_denoiser_history_ratio = 0.5f;
    float           ssdo_depth_bias             = 0.001f;
    float           ssdo_sample_radius          = 0.16f;
    float           ssdo_indirect_intensity     = 1.0f;
    float           ssdo_max_distance           = 0.5f;

    // MARK: SSR
    bool  ssr_is_ssr_enabled             = false;
    int   ssr_sample_count               = 32;
    bool  ssr_is_enable_jitter           = true;
    bool  ssr_is_force_ground_enable_ssr = true;
    float ssr_roughness_threshold        = 0.5;
    float ssr_metallic_threshold         = 0.5;
    float ssr_step_base                  = 0.025;

    // MARK: Denoiser
    EDenoiserMode denoiser_mode                     = EDenoiserMode::NONE;
    float         denoiser_bfd_spatial_sigma_square = 20.0f;  // [1, 200]
    float         denoiser_bfd_range_sigma_square   = 0.001f; // [0.01, 0.1]
    int           denoiser_bfd_kernel_radius        = 5;      // [1, 10]

    // MARK: AI (CUDA, TensorRT)
    bool        ai_is_cuda_enabled          = false;
    float       ai_cuda_pass_debug_param    = 1.0f;
    int         ai_trt_visualize_buffer_idx = s_ai_trt_visualize_buffer_array.size() - 2; // output
    std::string ai_trt_visualize_buffer =
        s_ai_trt_visualize_buffer_array[s_ai_trt_visualize_buffer_array.size() - 2];

    // MARK: Shadow
    int shadow_map_mode            = 1; // 0: disabled, 1: CSM, 2: VSM
    int shadow_sampling_mode       = 0;
    int shadow_csm_num_of_cascades = 2;
    int shadow_csm_sm_size         = 2048;

    StaticArray<float, CSM_MAX_CASCADES> shadow_csm_cover_ratio_of_camera = {0.01, 0.04, 0.32, 1.0};

    // MARK: Upsample Process
    EUpsampleMode upsample_mode = EUpsampleMode::None;
    int           outSize_x     = 1080;
    int           outSize_y     = 1920;
    int           inSize_x      = 540;

    // MARK: Debug
    bool  debug_fps_limit_enable = false;
    float debug_fps_limit        = 60;

    // MARK: Others
    std::string default_selected_frame_buffer_name = "aa_output";
    uint        selected_frame_buffer_index        = 0;
};

} // namespace Moer