#include "RaytracingUI.h"

#include "math/Function.h"
#include "misc/STL.h"
#include "shaderheaders/shared/ShaderParameters.h"
#include "renderer/raytracing/RaytracingConfig.h"

using namespace Moer::Render;

namespace Moer {

enum ELocalLightSelectionMode {
    ELLS_Uniform   = s_di_local_light_sample_mode_uniform,
    ELLS_Power_RIS = s_di_local_light_sample_mode_power_ris,
    ELLS_Grid      = s_di_local_light_sample_mode_grid,
    ELLS_Num
};

enum EGridLightPresampleMode {
    EGLPM_Uniform   = s_di_local_light_sample_mode_uniform,
    EGLPM_Power_RIS = s_di_local_light_sample_mode_power_ris,
    EGLPM_Num
};

enum EBiasCorrectionMode {
    EBCM_None      = s_di_bias_correction_none,
    EBCM_Basic     = s_di_bias_correction_basic,
    EBCM_Pair_Wise = s_di_bias_correction_pair_wise,
    EBCM_Traced    = s_di_bias_correction_traced,
    EBCM_Num
};

static constexpr std::string_view s_local_light_sample_mode_names[] = {"Uniform", "Power RIS", "Grid"};

static constexpr std::string_view s_grid_light_presample_mode_names[] = {"Uniform", "Power RIS"};

static constexpr std::string_view s_bias_correction_mode_names[] = {"None", "Basic", "Pair Wise", "Traced"};

static constexpr std::string_view s_denoiser_mode_names[] = {"None", "ReBlur", "Relax"};

static constexpr std::string_view s_aa_mode_names[]     = {"TAA"};
static constexpr std::string_view s_jitter_mode_names[] = {"MSAA", "Halton", "R2", "White Noise"};
static constexpr std::string_view s_final_color_names[] = {
    "SceneColor",
    "DI",
    "Emissive",
    "Diffuse",
    "Specular",
    "Normal",
    "ViewDepth",
    "Depth",
    "Motion",
    "Grid",
    "Material",
    "Position",
    "CameraPosition",
    "PrimaryRay",
    "ViewParam",
    "Custom"
};

static constexpr std::string_view s_exported_texture_names[] = {"LDR", "HDR"};

RaytracingUI::RaytracingUI(RaytracingConfig& config) : config(config) {
    final_color_map["SceneColor"] = EFinalColor::EFC_SceneColor;
    final_color_map["DI"]         = EFinalColor::EFC_DI;
    final_color_map["Emissive"]   = EFinalColor::EFC_EMISSIVE;
    final_color_map["Diffuse"]    = EFinalColor::EFC_DIFFUSE;
    final_color_map["Specular"]   = EFinalColor::EFC_SPECULAR;
    final_color_map["Normal"]     = EFinalColor::EFC_NORMAL;
    final_color_map["ViewDepth"]  = EFinalColor::EFC_VIEW_DEPTH;
    final_color_map["Depth"]      = EFinalColor::EFC_DEPTH;
    final_color_map["Motion"]     = EFinalColor::EFC_MOTION;
    final_color_map["Grid"]       = EFinalColor::EFC_GRID;
    final_color_map["Material"]   = EFinalColor::EFC_MATERIAL;
    final_color_map["Position"]   = EFinalColor::EFC_POSITION;
    final_color_map["CameraPosition"] = EFinalColor::EFC_CAMERA_POSITION;
    final_color_map["PrimaryRay"]     = EFinalColor::EFC_PRIMARY_RAY;
    final_color_map["ViewParam"]      = EFinalColor::EFC_VIEW_PARAM;
    final_color_map["Custom"]     = EFinalColor::EFC_CUSTOM;
}

void RaytracingUI::ShowConfig(Synapse::Context& ui) {
    if (!ui.TreeNode("Raytracing Config")) {
        return;
    }

    if (ui.TreeNode("Final Color")) {
        for (auto& [name, index] : final_color_map) {
            if (ui.Selectable(name.c_str(), config.final_color == index)) {
                config.final_color = static_cast<EFinalColor>(index);
            }
        }

        ui.TreePop();
    }
    ui.Separator();

    if (ui.TreeNode("Process Light Configs")) {
        ui.Checkbox("Parallel Mode", &config.process_light_cfg.parallel_mode);
        ui.SliderInt("Num Threads", &config.process_light_cfg.num_threads, 1, 32);
        ui.TreePop();
    }

    // Grid Config
    if (ui.TreeNode("Grid Config")) {

        if (ui.TreeNode("Presample Mode")) {
            for (auto& name : s_grid_light_presample_mode_names) {
                uint idx = &name - s_grid_light_presample_mode_names;
                if (ui.Selectable(name.data(), config.grid_config.grid_mode == idx)) {
                    config.grid_config.grid_mode = idx;
                }
            }
            ui.TreePop();
        }
        ui.SliderInt("Light Per Ceil", &config.grid_config.light_per_ceil, 1, 1024);
        ui.SliderFloat("Cell Size", &config.grid_config.cell_size, 1.f, 400.f);
        ui.TreePop();
    }
    ui.Separator();

    if (ui.TreeNode("ReSTIRDI")) {
        if (ui.TreeNode("InitialSampleSettings")) {
            if (ui.TreeNode("LocalLightSelection")) {
                for (auto& name : s_local_light_sample_mode_names) {
                    uint idx = &name - s_local_light_sample_mode_names;
                    if (ui.Selectable(
                            name.data(),
                            config.restir_di_cfg.initial_sample_config.local_light_sample_mode == idx
                        )) {
                        config.restir_di_cfg.initial_sample_config.local_light_sample_mode = idx;
                    }
                }
                ui.TreePop();
            }
            ui.TreePop();
        }

        if (ui.TreeNode("TemporalResampleSettings")) {
            if (ui.TreeNode("BiasCorrection")) {
                for (auto& name : s_bias_correction_mode_names) {
                    uint idx = &name - s_bias_correction_mode_names;
                    if (ui.Selectable(
                            name.data(), config.restir_di_cfg.temporal_resample_config.bias_correction == idx
                        )) {
                        config.restir_di_cfg.temporal_resample_config.bias_correction = idx;
                    }
                }
                ui.TreePop();
            }
            ui.SliderFloat(
                "depth threshold", &config.restir_di_cfg.temporal_resample_config.depth_threshold, 0.1f, 30.0f
            );
            ui.SliderFloat(
                "normal threshold",
                &config.restir_di_cfg.temporal_resample_config.normal_threshold,
                0.0f,
                1.0f
            );
            ui.TreePop();
        }

        if (ui.TreeNode("SpatialResampleSettings")) {
            if (ui.TreeNode("BiasCorrection")) {
                for (auto& name : s_bias_correction_mode_names) {
                    uint idx = &name - s_bias_correction_mode_names;
                    if (ui.Selectable(
                            name.data(), config.restir_di_cfg.spatial_resample_config.bias_correction == idx
                        )) {
                        config.restir_di_cfg.spatial_resample_config.bias_correction = idx;
                    }
                }
                ui.TreePop();
            }
            ui.SliderFloat(
                "depth threshold", &config.restir_di_cfg.spatial_resample_config.depth_threshold, 0.0f, 1.0f
            );
            ui.SliderFloat(
                "normal threshold", &config.restir_di_cfg.spatial_resample_config.normal_threshold, 0.0f, 1.0f
            );
            ui.SliderInt(
                "num spatial samples",
                &config.restir_di_cfg.spatial_resample_config.num_spatial_samples,
                1,
                32
            );
            ui.TreePop();
        }

        ui.TreePop();
    }
    ui.Separator();
    ui.Text("NRD Config");
    if (ui.TreeNode("DenoiserConfig")) {
        for (auto& name : s_denoiser_mode_names) {
            uint idx = &name - s_denoiser_mode_names;
            if (ui.Selectable(name.data(), config.denoiser_cfg.denoiser_type == idx)) {
                config.denoiser_cfg.denoiser_type = idx;
            }
        }
        ui.TreePop();
    }
    ui.Separator();
    ui.Text("Post-Process Config");
    ui.Checkbox("Enable ToneMapping", &config.tone_mapping_cfg.enable_tone_mapping);
    if (ui.TreeNode("ToneMapping")) {
        ui.SliderFloat(
            "Histogram Low Percentile", &config.tone_mapping_cfg.histogram_low_percentile, 0.0f, 1.0f
        );
        ui.SliderFloat(
            "Histogram High Percentile", &config.tone_mapping_cfg.histogram_high_percentile, 0.0f, 1.0f
        );
        ui.SliderFloat(
            "Eye Adaptation Speed Up", &config.tone_mapping_cfg.eye_adaptation_speed_up, 0.0f, 10.0f
        );
        ui.SliderFloat(
            "Eye Adaptation Speed Down", &config.tone_mapping_cfg.eye_adaptation_speed_down, 0.0f, 10.0f
        );
        ui.SliderFloat(
            "Min Adapted Luminance", &config.tone_mapping_cfg.min_adapted_luminance, 0.0f, 10.0f
        );
        ui.SliderFloat(
            "Max Adapted Luminance", &config.tone_mapping_cfg.max_adapted_luminance, 0.0f, 10.0f
        );
        ui.SliderFloat("Exposure Bias", &config.tone_mapping_cfg.exposure_bias, -10.0f, 10.0f);
        ui.SliderFloat("WhitePoint", &config.tone_mapping_cfg.white_point, 0.0f, 10.0f);
        ui.TreePop();
    }
    if (ui.TreeNode("AntiAlias")) {
        if (ui.TreeNode("AA Settings")) {
            for (uint i = 0; i < EAnitiAliasMode::EAA_Num; i++) {
                if (ui.Selectable(s_aa_mode_names[i].data(), config.aa_cfg.aa_mode == i)) {
                    config.aa_cfg.aa_mode = (EAnitiAliasMode)i;
                }
            }
            ui.TreePop();
        }

        if (ui.TreeNode("Jitter Settings")) {
            for (uint i = 0; i < uint(EJitter::WhiteNoise) + 1; i++) {
                if (ui.Selectable(s_jitter_mode_names[i].data(), uint(config.aa_cfg.jitter_mode) == i)) {
                    config.aa_cfg.jitter_mode = (EJitter)i;
                }
            }
            ui.TreePop();
        }
        ui.SliderFloat("New Frame Weight", &config.aa_cfg.new_frame_weight, 0.0f, 1.0f);
        ui.SliderFloat("Clamping Factor", &config.aa_cfg.clamping_factor, 0.0f, 10.0f);
        ui.SliderFloat("Max Radiance", &config.aa_cfg.max_radiance, 0.0f, 10000.0f);
        ui.Checkbox("Enable History Clamping", &config.aa_cfg.enable_history_clamping);
        ui.TreePop();
    }
    ui.Separator();
    ui.Text("Directional Light Config");
    config.sun_direction = Normalizef(config.sun_direction);
    ui.SliderFloat3("Sun Direction", &config.sun_direction.x, -1.0f, 1.0f);
    ui.SliderFloat("Exposure", &config.exposure, 0.0f, 10.0f);
    // ui.SliderFloat("Sun Angular Diameter", &config.sun_angular_diameter,
    // 0.0f, 1.0f);

    int max_bounce = config.max_bounce;
    // ui.SliderInt("Max Bounce", &max_bounce, 1, 5);
    config.max_bounce = max_bounce;

    ui.Separator();
    ui.Text("Capture Settings");
    for (uint i = 0; i < EOutputTexture::EOT_Num; i++) {
        if (ui.Selectable(s_exported_texture_names[i].data(), config.export_cfg.output_texture == i)) {
            config.export_cfg.output_texture = (EOutputTexture)i;
        }
    }
    if (ui.Button("Capture Screen")) {
        config.export_cfg.b_export = true;
    }

    ui.TreePop();
}

} // namespace Moer
