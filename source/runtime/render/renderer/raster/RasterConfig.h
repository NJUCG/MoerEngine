#pragma once

#include "misc/STL.h"
#include "misc/Traits.h"

#include "RasterCompileTimeConstants.h"
#include "shaderheaders/shared/raster/ShaderParameters.h"

#include <string>

namespace Moer {

// EnumParam(EShadingMode, DEFAULT, DEBUG);
static const UnorderedMap<EShadingMode, std::string> s_shading_mode_name_map = {
    {EShadingMode::DEFAULT_PBR, "Default PBR (GAMES202)"},
    {EShadingMode::DEBUG, "Debug"},
};

// EnumParam(EBrdfNdfMode, BECKMANN, GGX, EXTENDING_GGX);
static const UnorderedMap<EBrdfNdfMode, std::string> s_brdf_ndf_mode_name_map = {
    {EBrdfNdfMode::BECKMANN, "Beckmann"},
    {EBrdfNdfMode::GGX, "GGX"},
    {EBrdfNdfMode::GTR2, "GTR Gamma=2"},
    {EBrdfNdfMode::GTR1, "GTR Gamma=1"},
};

// EnumParam(EBrdfGMode, G_SCHLICK, VIS_UE4, VIS_UNITY, VIS_FILAMENT, VIS_RESPAWN);
static const UnorderedMap<EBrdfGMode, std::string> s_brdf_geometry_mode_name_map = {
    {EBrdfGMode::G_SCHLICK, "GGX-Smith Separable (UE4)"},
    {EBrdfGMode::VIS_UE4, "GGX-Smith Joint (UE4)"},
    {EBrdfGMode::VIS_UNITY, "GGX-Smith Joint (Unity)"},
    {EBrdfGMode::VIS_FILAMENT, "GGX-Smith Joint (Filament)"},
    {EBrdfGMode::VIS_RESPAWN, "GGX-Smith Joint (Respawn E.)"},
};

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

// EnumParam(EShadowMapMode, NONE, CSM, CSM_AUTO);
static const UnorderedMap<EShadowMapMode, std::string> s_shadow_map_mode_name_map = {
    {EShadowMapMode::NONE, "None"},
    {EShadowMapMode::POINT_CUBE, "Point Cube"},
    {EShadowMapMode::CSM, "CSM"},
    {EShadowMapMode::CSM_AUTO, "CSM Auto"},
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

static const Array<std::string> s_rtao_sample_mode = {
    "Uniform in Semisphere",
    "Cosine-Weighted in Semisphere"
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

struct CooperativeOpsStatus {
    bool extension_enabled                     = false;
    bool inference_ready                       = false;
    bool matrix_supported                      = false;
    bool matrix_robust_buffer_access_supported = false;
    bool vector_supported                      = false;
    bool vector_training_supported             = false;
    bool low_precision_supported               = false;
    bool storage_supported                     = false;
    bool vulkan_memory_model_supported         = false;

    uint matrix_mode_count       = 0;
    uint vector_mode_count       = 0;
    uint matrix_supported_stages = 0;
    uint vector_supported_stages = 0;
    uint max_vector_components   = 0;
    uint frames_evaluated        = 0;

    std::string overview              = "Waiting for cooperative snapshot...";
    std::string matrix_summary        = "Waiting for cooperative snapshot...";
    std::string matrix_runtime_status = "Idle";
    std::string vector_summary        = "Waiting for cooperative snapshot...";
    std::string vector_runtime_status = "Idle";
};

struct RasterConfig {

    // MARK: Geometry

    bool  geometry_enable_alpha_test             = true;
    float geometry_alpha_test_blend_pixel_cutoff = 0.5f; // 当AlphaMode为BLEND时，低于该值的像素会被丢弃
    bool  enable_frustum_culling                 = true; // GPU视锥剔除

    // MARK: Culling Statistics (只读，由GPU更新)
    struct CullingStats {
        uint total_instances_before = 0;
        uint total_instances_after  = 0;
        uint visible_draws          = 0;
        uint total_draws            = 0;

        float GetCullingRatio() const {
            if (total_instances_before == 0)
                return 0.0f;
            return 1.0f - (float)total_instances_after / (float)total_instances_before;
        }
    } culling_stats;

    // MARK: Shading
    EShadingMode shading_mode = EShadingMode::DEFAULT_PBR;

    bool   shading_enable_extra_ambient    = false;
    float3 shading_extra_ambient_color     = float3(1.f, 1.f, 1.f);
    float  shading_extra_ambient_intensity = 0.01f;

    bool         shading_brdf_enable_multi_scatter = true; // kulla-conty approximation
    EBrdfNdfMode shading_brdf_NDF_mode             = EBrdfNdfMode::GGX;
    EBrdfGMode   shading_brdf_G_mode               = EBrdfGMode::VIS_UE4;
    bool         shading_brdf_G_is_ibl             = false; // 是否使用IBL的Fresnel近似

    // MARK: Tonemapping
    float tonemapping_exposure_ev      = -2.7f;
    bool  tonemapping_reinhard_enabled = true;

    struct TonemappingAutoExposureConfig {
        bool enabled = true;

        float log2lum_min = -10.0f;
        float log2lum_max = 16.0f;

        float histogram_low_percentile  = 0.5f;
        float histogram_high_percentile = 0.9f;

        float min_adapted_luminance = 1e-3f;
        float max_adapted_luminance = 5e3f;

        float eye_adaptation_speed_up   = 2.0f;
        float eye_adaptation_speed_down = 2.0f;

        float diff_log2_threshold = 0.05f; // 小于这个值，则直接使用目标曝光值，避免过度缓慢变化

        bool aces_tonemapping_enabled = true;
        bool debug_visualize          = false;
    } tonemapping_ae;

    // MARK: AA
    EAaMode aa_mode = EAaMode::SMAA_1X;

    // MARK: AO
    EAoMode ao_mode            = EAoMode::RTAO;
    bool    ao_half_resolution = true;

    float ssao_intensity     = 1.0f;
    int   ssao_spp           = 16;
    int   ssao_sample_radius = 16;
    float ssao_max_distance  = 1.0f;

    ERtaoSampleMode rtao_sample_mode        = ERtaoSampleMode::COSINE_WEIGHTED;
    float           rtao_intensity          = 1.0f;
    float           rtao_ray_trace_distance = 1.0f;
    int             rtao_spp                = 8;

    bool  rtao_denoiser_enable                  = true;
    bool  rtao_denoiser_reprojection_enable     = true;
    bool  rtao_denoiser_validation_enable       = true;
    bool  rtao_denoiser_history_clamp_enable    = true;
    bool  rtao_denoiser_motion_weighting_enable = true;
    float rtao_denoiser_history_ratio           = 0.9f;
    float rtao_denoiser_valid_depth_threshold   = 0.01f;
    float rtao_denoiser_valid_normal_threshold  = 0.8f;

    float ssdo_depth_bias         = 0.001f;
    float ssdo_sample_radius      = 0.16f;
    float ssdo_indirect_intensity = 1.0f;
    float ssdo_max_distance       = 0.5f;

    // MARK: SSR
    bool  ssr_is_ssr_enabled             = false;
    int   ssr_sample_count               = 32;
    bool  ssr_is_enable_jitter           = true;
    bool  ssr_is_force_ground_enable_ssr = true;
    float ssr_roughness_threshold        = 0.5;
    float ssr_metallic_threshold         = 0.5;
    float ssr_step_base                  = 0.025;

    // MARK: Bloom
    bool bloom_enabled = true;

    // MARK: Cooperative Ops
    bool                 cooperative_ops_enabled = true;
    CooperativeOpsStatus cooperative_ops_status{};

    // MARK: Denoiser
    EDenoiserMode denoiser_mode                     = EDenoiserMode::NONE;
    float         denoiser_bfd_spatial_sigma_square = 20.0f;  // [1, 200]
    float         denoiser_bfd_range_sigma_square   = 0.001f; // [0.01, 0.1]
    int           denoiser_bfd_kernel_radius        = 5;      // [1, 10]

    // MARK: AI (CUDA, TensorRT)
    bool        ai_is_cuda_enabled          = false;
    int         ai_trt_visualize_buffer_idx = s_ai_trt_visualize_buffer_array.size() - 2; // output
    std::string ai_trt_visualize_buffer =
        s_ai_trt_visualize_buffer_array[s_ai_trt_visualize_buffer_array.size() - 2];
    bool ai_trt_force_ldr = true;

    // MARK: Shadow
    EShadowMapMode shadow_map_mode                       = EShadowMapMode::CSM;
    int            shadow_sampling_mode                  = 0;
    int            shadow_csm_num_of_cascades            = 4;
    float          shadow_csm_lerp_factor                = 0.015f;
    float          shadow_csm_blend_percentage           = 0.3f;
    bool           shadow_csm_blend_option               = true;
    int            shadow_csm_sm_size                    = 4096;
    bool           shadow_csm_visualize_cascade          = false;
    bool           shadow_cache_enabled                  = true; // 开启后允许远级联复用上一帧阴影图
    int            shadow_cache_disable_first_n_cascades = 0;    // 前N层级联始终全量刷新

    // MARK: Shadow - PCSS
    bool  shadow_pcss_enabled          = true;
    float shadow_pcss_light_size_world = 0.01f;

    StaticArray<float, CSM_MAX_CASCADES> shadow_csm_cover_ratio_of_camera = {0.005, 0.02, 0.1, 0.25};
    StaticArray<float, CSM_MAX_CASCADES> shadow_cache_camera_move_threshold_in_texels =
        {1.0f, 8.0f, 32.0f, 128.0f}; // 级联中心移动超过该阈值后刷新

    // MARK: Skybox
    bool  skybox_exposure_correct_enabled      = true;         // 启用的话，就会找到第一个平行光，乘上它的颜色
    float skybox_exposure_correct_factor_log10 = log10f(0.5f); // 曝光校正因子

    // MARK: Upsample Process
    EUpsampleMode upsample_mode = EUpsampleMode::None;
    int           outSize_x     = 1080;
    int           outSize_y     = 1920;
    int           inSize_x      = 540;

    // MARK: Debug
    float debug_param            = 1.0f;
    bool  debug_fps_limit_enable = false;
    float debug_fps_limit        = 60;

    // MARK: Others

    uint  selected_frame_buffer_index = 0;
    float frame_time                  = 1.0f / 60.0f; // default 0.0167s
};

} // namespace Moer