#pragma once

// 定义 Raytracing 管线可由编辑器调整的运行时配置。

#include "misc/Traits.h"
#include "shaderheaders/shared/ShaderParameters.h"

namespace Moer {

enum EAnitiAliasMode {
    EAA_TAA = 0,
    EAA_Num
};
enum EOutputTexture {
    EOT_LDR = 0,
    EOT_HDR,
    EOT_Num
};
enum class EJitter {
    MSAA,
    Halton,
    R2,
    WhiteNoise,
    Num
};

struct GridConfig {
    int grid_mode = 1;
    // 旧字段已公开，保留为唯一存储；新代码通过正确拼写的访问器读写。
    int   light_per_ceil = 512;
    float cell_size      = 1.f;

    int& GetLightsPerCell() {
        return light_per_ceil;
    }

    int GetLightsPerCell() const {
        return light_per_ceil;
    }

    void SetLightsPerCell(int lights_per_cell) {
        light_per_ceil = lights_per_cell;
    }
};

struct ReSTIRDIInitialSampleConfig {
    uint local_light_sample_mode                = Render::s_di_local_light_sample_mode_grid;
    bool enable_adaptive_local_light_sampling   = true;
    int  grid_min_local_light_count             = 64;
};

struct ReSTIRDITemporalResampleConfig {
    uint  bias_correction  = Render::s_di_bias_correction_traced;
    float depth_threshold  = 10.f;
    float normal_threshold = 0.5f;
};

struct ReSTIRDISpatialResampleConfig {
    uint  bias_correction     = Render::s_di_bias_correction_traced;
    float depth_threshold     = 0.2f;
    float normal_threshold    = 0.5f;
    int   num_spatial_samples = 2;
};

struct ReSTIRDIConfig {
    ReSTIRDIInitialSampleConfig    initial_sample_config;
    ReSTIRDITemporalResampleConfig temporal_resample_config;
    ReSTIRDISpatialResampleConfig  spatial_resample_config;
};
struct ReBlurHitDistParams {
    float A = 3.f;
    float B = 0.1f;
    float C = 20.f;
    float D = -25.f;
};

struct ReBlurAntilagParams {
    float luminance_sigma_scale = 2.f;
    float hit_dist_sigma_scale  = 2.f;

    float luminance_antilag_power = .5f;
    float hit_dist_antilag_power  = 1.f;
};
struct DenoiserConfig {
    ReBlurHitDistParams hit_dist_params{};
    ReBlurAntilagParams antilag_params{};
    uint                denoiser_type = Render::s_denoiser_mode_relax;
};

struct ToneMappingConfig {
    float histogram_low_percentile  = 0.8f;
    float histogram_high_percentile = 0.95f;
    float eye_adaptation_speed_up   = 1.f;
    float eye_adaptation_speed_down = 0.5f;
    float min_adapted_luminance     = 0.02f;
    float max_adapted_luminance     = 0.5f;
    float exposure_bias             = -0.5f;
    float white_point               = 1.5f;
    bool  enable_color_lut          = true;
    bool  enable_tone_mapping       = true;
};

struct AntiAliasConfig {
    EAnitiAliasMode aa_mode = EAnitiAliasMode::EAA_TAA;

    EJitter jitter_mode = EJitter::MSAA;

    float new_frame_weight        = 0.04f;
    float clamping_factor         = 1.3f;
    float max_radiance            = 200.f;
    bool  enable_history_clamping = true;
};

struct ExportConfig {
    EOutputTexture output_texture = EOutputTexture::EOT_LDR;
    bool           b_export       = false;
};

struct RTProcessLightConfig {
    bool parallel_mode = 0;
    int  num_threads   = 4;
};
struct RaytracingConfig {
    float3 sun_direction               = float3(0.f, 0.5f, 0.16f);
    float  directional_light_intensity = 6.f;
    float  sun_angular_diameter        = 0.533f;
    uint   max_bounce                  = 4;

    GridConfig           grid_config{};
    ReSTIRDIConfig       restir_di_cfg{};
    DenoiserConfig       denoiser_cfg{};
    ToneMappingConfig    tone_mapping_cfg{};
    AntiAliasConfig      aa_cfg{};
    ExportConfig         export_cfg{};
    RTProcessLightConfig process_light_cfg{};
    Render::EFinalColor  final_color = Render::EFinalColor::EFC_SceneColor;
};

} // namespace Moer
