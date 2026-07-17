#include "RaytracingUI.h"

// 提供 ReSTIR DI、降噪、后处理和捕获配置的控制项。

#include "math/Function.h"

#include <imgui.h>

#include <array>
#include <string_view>

using namespace Moer::Render;

namespace Moer {

namespace {

constexpr std::array<std::string_view, 3> k_local_light_sample_mode_names = {
    "Uniform", "Power RIS", "Grid"
};
constexpr std::array<std::string_view, 2> k_grid_light_presample_mode_names = {
    "Uniform", "Power RIS"
};
constexpr std::array<std::string_view, 4> k_bias_correction_mode_names = {
    "None", "Basic", "Pair Wise", "Traced"
};
constexpr std::array<std::string_view, 3> k_denoiser_mode_names = {"None", "ReBlur", "Relax"};
constexpr std::array<std::string_view, 1> k_aa_mode_names       = {"TAA"};
constexpr std::array<std::string_view, 4> k_jitter_mode_names = {
    "MSAA", "Halton", "R2", "White Noise"
};
constexpr std::array<std::string_view, 2> k_exported_texture_names = {"LDR", "HDR"};

} // namespace

RaytracingUI::RaytracingUI(RaytracingConfig& config) : m_config(config) {
    m_final_color_options["SceneColor"] = EFinalColor::EFC_SceneColor;
    m_final_color_options["DI"]         = EFinalColor::EFC_DI;
    m_final_color_options["Emissive"]   = EFinalColor::EFC_EMISSIVE;
    m_final_color_options["Diffuse"]    = EFinalColor::EFC_DIFFUSE;
    m_final_color_options["Specular"]   = EFinalColor::EFC_SPECULAR;
    m_final_color_options["Normal"]     = EFinalColor::EFC_NORMAL;
    m_final_color_options["ViewDepth"]  = EFinalColor::EFC_VIEW_DEPTH;
    m_final_color_options["Depth"]      = EFinalColor::EFC_DEPTH;
    m_final_color_options["Motion"]     = EFinalColor::EFC_MOTION;
    m_final_color_options["Grid"]       = EFinalColor::EFC_GRID;
    m_final_color_options["Material"]   = EFinalColor::EFC_MATERIAL;
    m_final_color_options["Position"]   = EFinalColor::EFC_POSITION;
    m_final_color_options["Custom"]     = EFinalColor::EFC_CUSTOM;
}

void RaytracingUI::ShowConfig() {
    if (!ImGui::TreeNode("Raytracing Config")) {
        return;
    }

    if (ImGui::TreeNode("Final Color")) {
        for (const auto& [name, value] : m_final_color_options) {
            if (ImGui::Selectable(name.c_str(), m_config.final_color == value)) {
                m_config.final_color = static_cast<EFinalColor>(value);
            }
        }

        ImGui::TreePop();
    }
    ImGui::Separator();

    if (ImGui::TreeNode("Process Light Configs")) {
        ImGui::Checkbox("Parallel Mode", &m_config.process_light_cfg.parallel_mode);
        ImGui::SliderInt("Num Threads", &m_config.process_light_cfg.num_threads, 1, 32);
        ImGui::TreePop();
    }

    // Grid Config
    if (ImGui::TreeNode("Grid Config")) {

        if (ImGui::TreeNode("Presample Mode")) {
            for (uint index = 0; index < k_grid_light_presample_mode_names.size(); ++index) {
                if (ImGui::Selectable(
                        k_grid_light_presample_mode_names[index].data(),
                        m_config.grid_config.grid_mode == index
                    )) {
                    m_config.grid_config.grid_mode = index;
                }
            }
            ImGui::TreePop();
        }
        ImGui::SliderInt("Lights per Cell", &m_config.grid_config.GetLightsPerCell(), 1, 1024);
        ImGui::SliderFloat("Cell Size", &m_config.grid_config.cell_size, 1.f, 400.f);
        ImGui::TreePop();
    }
    ImGui::Separator();

    if (ImGui::TreeNode("ReSTIRDI")) {
        if (ImGui::TreeNode("InitialSampleSettings")) {
            if (ImGui::TreeNode("LocalLightSelection")) {
                for (uint index = 0; index < k_local_light_sample_mode_names.size(); ++index) {
                    if (ImGui::Selectable(
                            k_local_light_sample_mode_names[index].data(),
                            m_config.restir_di_cfg.initial_sample_config.local_light_sample_mode == index
                        )) {
                        m_config.restir_di_cfg.initial_sample_config.local_light_sample_mode = index;
                    }
                }
                ImGui::Checkbox(
                    "Adaptive Grid Fallback",
                    &m_config.restir_di_cfg.initial_sample_config.enable_adaptive_local_light_sampling
                );
                if (m_config.restir_di_cfg.initial_sample_config.enable_adaptive_local_light_sampling) {
                    ImGui::SliderInt(
                        "Grid Min Local Lights",
                        &m_config.restir_di_cfg.initial_sample_config.grid_min_local_light_count,
                        1,
                        1024
                    );
                }
                ImGui::TreePop();
            }
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("TemporalResampleSettings")) {
            if (ImGui::TreeNode("BiasCorrection")) {
                for (uint index = 0; index < k_bias_correction_mode_names.size(); ++index) {
                    if (ImGui::Selectable(
                            k_bias_correction_mode_names[index].data(),
                            m_config.restir_di_cfg.temporal_resample_config.bias_correction == index
                        )) {
                        m_config.restir_di_cfg.temporal_resample_config.bias_correction = index;
                    }
                }
                ImGui::TreePop();
            }
            ImGui::SliderFloat(
                "depth threshold",
                &m_config.restir_di_cfg.temporal_resample_config.depth_threshold,
                0.1f,
                30.0f
            );
            ImGui::SliderFloat(
                "normal threshold",
                &m_config.restir_di_cfg.temporal_resample_config.normal_threshold,
                0.0f,
                1.0f
            );
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("SpatialResampleSettings")) {
            if (ImGui::TreeNode("BiasCorrection")) {
                for (uint index = 0; index < k_bias_correction_mode_names.size(); ++index) {
                    if (ImGui::Selectable(
                            k_bias_correction_mode_names[index].data(),
                            m_config.restir_di_cfg.spatial_resample_config.bias_correction == index
                        )) {
                        m_config.restir_di_cfg.spatial_resample_config.bias_correction = index;
                    }
                }
                ImGui::TreePop();
            }
            ImGui::SliderFloat(
                "depth threshold",
                &m_config.restir_di_cfg.spatial_resample_config.depth_threshold,
                0.0f,
                1.0f
            );
            ImGui::SliderFloat(
                "normal threshold",
                &m_config.restir_di_cfg.spatial_resample_config.normal_threshold,
                0.0f,
                1.0f
            );
            ImGui::SliderInt(
                "num spatial samples",
                &m_config.restir_di_cfg.spatial_resample_config.num_spatial_samples,
                1,
                32
            );
            ImGui::TreePop();
        }

        ImGui::TreePop();
    }
    ImGui::Separator();
    ImGui::Text("NRD Config");
    if (ImGui::TreeNode("DenoiserConfig")) {
        for (uint index = 0; index < k_denoiser_mode_names.size(); ++index) {
            if (ImGui::Selectable(
                    k_denoiser_mode_names[index].data(), m_config.denoiser_cfg.denoiser_type == index
                )) {
                m_config.denoiser_cfg.denoiser_type = index;
            }
        }
        ImGui::TreePop();
    }
    ImGui::Separator();
    ImGui::Text("Post-Process Config");
    ImGui::Checkbox("Enable ToneMapping", &m_config.tone_mapping_cfg.enable_tone_mapping);
    if (ImGui::TreeNode("ToneMapping")) {
        ImGui::SliderFloat(
            "Histogram Low Percentile",
            &m_config.tone_mapping_cfg.histogram_low_percentile,
            0.0f,
            1.0f
        );
        ImGui::SliderFloat(
            "Histogram High Percentile",
            &m_config.tone_mapping_cfg.histogram_high_percentile,
            0.0f,
            1.0f
        );
        ImGui::SliderFloat(
            "Eye Adaptation Speed Up",
            &m_config.tone_mapping_cfg.eye_adaptation_speed_up,
            0.0f,
            10.0f
        );
        ImGui::SliderFloat(
            "Eye Adaptation Speed Down",
            &m_config.tone_mapping_cfg.eye_adaptation_speed_down,
            0.0f,
            10.0f
        );
        ImGui::SliderFloat(
            "Min Adapted Luminance",
            &m_config.tone_mapping_cfg.min_adapted_luminance,
            0.0f,
            10.0f
        );
        ImGui::SliderFloat(
            "Max Adapted Luminance",
            &m_config.tone_mapping_cfg.max_adapted_luminance,
            0.0f,
            10.0f
        );
        ImGui::SliderFloat("Exposure Bias", &m_config.tone_mapping_cfg.exposure_bias, -10.0f, 10.0f);
        ImGui::SliderFloat("WhitePoint", &m_config.tone_mapping_cfg.white_point, 0.0f, 10.0f);
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("AntiAlias")) {
        if (ImGui::TreeNode("AA Settings")) {
            for (uint index = 0; index < EAnitiAliasMode::EAA_Num; ++index) {
                if (ImGui::Selectable(
                        k_aa_mode_names[index].data(), m_config.aa_cfg.aa_mode == index
                    )) {
                    m_config.aa_cfg.aa_mode = static_cast<EAnitiAliasMode>(index);
                }
            }
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Jitter Settings")) {
            for (uint index = 0; index < static_cast<uint>(EJitter::WhiteNoise) + 1; ++index) {
                if (ImGui::Selectable(
                        k_jitter_mode_names[index].data(),
                        static_cast<uint>(m_config.aa_cfg.jitter_mode) == index
                    )) {
                    m_config.aa_cfg.jitter_mode = static_cast<EJitter>(index);
                }
            }
            ImGui::TreePop();
        }
        ImGui::SliderFloat("New Frame Weight", &m_config.aa_cfg.new_frame_weight, 0.0f, 1.0f);
        ImGui::SliderFloat("Clamping Factor", &m_config.aa_cfg.clamping_factor, 0.0f, 10.0f);
        ImGui::SliderFloat("Max Radiance", &m_config.aa_cfg.max_radiance, 0.0f, 10000.0f);
        ImGui::Checkbox("Enable History Clamping", &m_config.aa_cfg.enable_history_clamping);
        ImGui::TreePop();
    }
    ImGui::Separator();
    ImGui::Text("Directional Light Config");
    m_config.sun_direction = Normalizef(m_config.sun_direction);
    ImGui::SliderFloat3("Sun Direction", &m_config.sun_direction.x, -1.0f, 1.0f);
    ImGui::SliderFloat("Exposure", &m_config.exposure, 0.0f, 10.0f);

    ImGui::Separator();
    ImGui::Text("Capture Settings");
    for (uint index = 0; index < EOutputTexture::EOT_Num; ++index) {
        if (ImGui::Selectable(
                k_exported_texture_names[index].data(), m_config.export_cfg.output_texture == index
            )) {
            m_config.export_cfg.output_texture = static_cast<EOutputTexture>(index);
        }
    }
    if (ImGui::Button("Capture Screen")) {
        m_config.export_cfg.b_export = true;
    }

    ImGui::TreePop();
}

} // namespace Moer
