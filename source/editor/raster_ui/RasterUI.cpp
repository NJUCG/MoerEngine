#include "RasterUI.h"

#include "config/ConfigManager.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <string_view>

namespace Moer {

RasterUI::RasterUI(RasterConfig& config) : m_config(config) {
    if (ConfigManager::GetInstance().GetConfig().engine.render.raster.low_quality_mode) {
        m_config.ao_mode = EAoMode::SSAO;

        m_config.shadow_map_mode                     = EShadowMapMode::CSM;
        m_config.shadow_csm_num_of_cascades          = 1;
        m_config.shadow_csm_sm_size                  = 2048;
        m_config.shadow_csm_cover_ratio_of_camera[0] = 0.03f;

        m_config.aa_mode = EAaMode::FXAA_SIMPLIFIED;
    }

    if (ConfigManager::GetInstance().GetConfig().engine.render.raster.enable_shadow == false) {
        m_config.shadow_map_mode = EShadowMapMode::NONE;
    }
}

void RasterUI::ShowConfig() {
    if (!ImGui::TreeNode("Raster Settings")) {
        return;
    }

    auto draw_border = [&]() {
        // 获取选项的矩形区域
        ImVec2 min = ImGui::GetItemRectMin();
        ImVec2 max = ImGui::GetItemRectMax();
        // 绘制边框
        ImGui::GetWindowDrawList()->AddRect(min, max, IM_COL32(255, 255, 255, 255));
    };

    if (ImGui::TreeNode(
            "Output Frame Buffer",
            "Output: [%s]",
            m_frame_buffer_and_name_array[m_config.selected_frame_buffer_index].GetTexture()->GetName().data()
        )) {
        for (uint i = 0; i < m_frame_buffer_and_name_array.size(); i++) {
            if (ImGui::Selectable(
                    m_frame_buffer_and_name_array[i].GetTexture()->GetName().data(),
                    m_config.selected_frame_buffer_index == i
                )) {
                m_config.selected_frame_buffer_index = i;
            }
            draw_border();
        }
        ImGui::TreePop();
    }

    // MARK: Geometry & Culling
    if (ImGui::TreeNode("Geometry & Culling")) {

        ImGui::Checkbox("Enable GPU Frustum Culling", &m_config.enable_frustum_culling);

        // Culling Statistics
        if (m_config.enable_frustum_culling) {
            ImGui::Separator();
            ImGui::Text("Culling Statistics:");
            ImGui::Indent();

            if (m_config.culling_stats.total_instances_before == 0) {
                ImGui::TextDisabled("  Waiting for data...");
            } else {
                const auto& stats      = m_config.culling_stats;
                uint32_t    culled     = stats.total_instances_before - stats.total_instances_after;
                float       culled_pct = 100.0f * float(culled) / float(stats.total_instances_before);
                ImGui::Text(
                    "Instances: %u / %u visible (%u culled, %.1f%%)",
                    stats.total_instances_after,
                    stats.total_instances_before,
                    culled,
                    culled_pct
                );
                ImGui::ProgressBar(culled_pct / 100.0f, ImVec2(150, 0));
            }
            ImGui::Unindent();
            ImGui::Separator();
        }

        ImGui::Checkbox("Enable Alpha Test", &m_config.geometry_enable_alpha_test);
        ImGui::SliderFloat("Alpha Cutoff", &m_config.geometry_alpha_test_blend_pixel_cutoff, 0.0f, 1.0f);

        ImGui::TreePop();
    }

    // MARK: Shading
    if (ImGui::TreeNode(
            "Shading", "Shading: [%s]", s_shading_mode_name_map.at(m_config.shading_mode).c_str()
        )) {

        // 有多个ShadingMode的时候，再显示这个选项
        // for (uint i = 0; i < s_shading_mode_name_map.size(); i++) {
        //     auto cur_enum = static_cast<EShadingMode>(i);
        //     if (ImGui::Selectable(
        //             s_shading_mode_name_map.at(cur_enum).c_str(), m_config.shading_mode == cur_enum
        //         )) {
        //         m_config.shading_mode = cur_enum;
        //     }
        //     draw_border();
        // }
        // ImGui::Separator();

        if (m_config.shading_mode == EShadingMode::DEFAULT_PBR) {
            // TODO: 添加注释，不然没人知道这些设置有什么用
            ImGui::Text("BRDF Settings:");

            // BRDF, Multi-Scatter
            ImGui::Checkbox("MultiScatter (Kulla-Conty)", &m_config.shading_brdf_enable_multi_scatter);

            // Spacing
            ImGui::Dummy(ImVec2(0.0f, 5.0f));

            // NDF
            ImGui::Text("NDF:");
            for (uint i = 0; i < s_brdf_ndf_mode_name_map.size(); i++) {
                auto cur_enum = static_cast<EBrdfNdfMode>(i);
                if (ImGui::Selectable(
                        s_brdf_ndf_mode_name_map.at(cur_enum).c_str(),
                        m_config.shading_brdf_NDF_mode == cur_enum
                    )) {
                    m_config.shading_brdf_NDF_mode = cur_enum;
                }
                draw_border();
            }

            // Spacing
            ImGui::Dummy(ImVec2(0.0f, 5.0f));

            // Geometry Function
            ImGui::Text("Geometry Function:");
            for (uint i = 0; i < s_brdf_geometry_mode_name_map.size(); i++) {
                auto cur_enum = static_cast<EBrdfGMode>(i);
                if (ImGui::Selectable(
                        s_brdf_geometry_mode_name_map.at(cur_enum).c_str(),
                        m_config.shading_brdf_G_mode == cur_enum
                    )) {
                    m_config.shading_brdf_G_mode = cur_enum;
                }
                draw_border();
            }
            if (m_config.shading_brdf_G_mode == EBrdfGMode::G_SCHLICK) {
                // Light Source is IBL
                ImGui::Checkbox("Light Source is IBL", &m_config.shading_brdf_G_is_ibl);
            }
            ImGui::Dummy(ImVec2(0.0f, 5.0f));
        }

        ImGui::Separator();
        ImGui::Dummy(ImVec2(0.0f, 5.0f));

        ImGui::Checkbox("Enable Extra Ambient", &m_config.shading_enable_extra_ambient);
        ImGui::SliderFloat("Ambient Intensity", &m_config.shading_extra_ambient_intensity, 0.0f, 1.0f);
        ImGui::SliderFloat3("Ambient Color", (float*)&m_config.shading_extra_ambient_color, 0.0f, 1.0f);

        ImGui::TreePop();
    }

    // MARK: Tonemapping
    // Tonemapping究极重要，所以放到最开头，以提示用户调节选项
    if (ImGui::TreeNode("Tonemapping")) {

        ImGui::Checkbox("Enable Auto Exposure", &m_config.tonemapping_ae.enabled);

        // 自动曝光或手动曝光都可以调整Exposure EV
        ImGui::SliderFloat("Exposure EV", &m_config.tonemapping_exposure_ev, -15.0f, 10.0f);

        // 自动曝光
        if (m_config.tonemapping_ae.enabled) {

            ImGui::Checkbox("Visualize (Try this!)", &m_config.tonemapping_ae.debug_visualize);

            ImGui::Checkbox("Enable ACES ToneMapping", &m_config.tonemapping_ae.aces_tonemapping_enabled);

            ImGui::SliderFloat("Luminance(log2) Min", &m_config.tonemapping_ae.log2lum_min, -20.0f, 5.0f);
            ImGui::SliderFloat("Luminance(log2) Max", &m_config.tonemapping_ae.log2lum_max, -5.0f, 20.0f);
            m_config.tonemapping_ae.log2lum_min =
                Min(m_config.tonemapping_ae.log2lum_min, m_config.tonemapping_ae.log2lum_max);
            m_config.tonemapping_ae.log2lum_max =
                Max(m_config.tonemapping_ae.log2lum_min, m_config.tonemapping_ae.log2lum_max);

            ImGui::SliderFloat(
                "Histogram Low Percentile", &m_config.tonemapping_ae.histogram_low_percentile, 0.0f, 1.0f
            );
            ImGui::SliderFloat(
                "Histogram High Percentile", &m_config.tonemapping_ae.histogram_high_percentile, 0.0f, 1.0f
            );
            m_config.tonemapping_ae.histogram_low_percentile =
                Min(m_config.tonemapping_ae.histogram_low_percentile,
                    m_config.tonemapping_ae.histogram_high_percentile);
            m_config.tonemapping_ae.histogram_high_percentile =
                Max(m_config.tonemapping_ae.histogram_low_percentile,
                    m_config.tonemapping_ae.histogram_high_percentile);

            ImGui::SliderFloat(
                "Eye Adaptation Speed (Up)", &m_config.tonemapping_ae.eye_adaptation_speed_up, 0.1f, 10.0f
            );
            ImGui::SliderFloat(
                "Eye Adaptation Speed (Down)", &m_config.tonemapping_ae.eye_adaptation_speed_down, 0.1f, 10.0f
            );

        } else {

            // 手动曝光
            ImGui::Checkbox("Enable Reinhard Tone Mapping", &m_config.tonemapping_reinhard_enabled);
        }

        ImGui::TreePop();
    }

    // MARK: Shadow
    if (ImGui::TreeNode(
            "Shadow", "Shadow: [%s]", s_shadow_map_mode_name_map.at(m_config.shadow_map_mode).c_str()
        )) {

        ImGui::Text("Only project 1st Dir.Light");

        for (uint i = 0; i < s_shadow_map_mode_name_map.size(); i++) {
            EShadowMapMode cur_enum = static_cast<EShadowMapMode>(i);
            if (ImGui::Selectable(
                    s_shadow_map_mode_name_map.at(cur_enum).c_str(), m_config.shadow_map_mode == cur_enum
                )) {
                m_config.shadow_map_mode = cur_enum;
            }
            draw_border();
        }

        ImGui::Checkbox("Enable PCSS", &m_config.shadow_pcss_enabled);
        if (m_config.shadow_pcss_enabled) {
            ImGui::SliderFloat("Light Size World", &m_config.shadow_pcss_light_size_world, 0.001f, 0.1f);
        }

        auto csm_common_param = [&]() {
            ImGui::SliderInt("Num of Cascades", &m_config.shadow_csm_num_of_cascades, 1, CSM_MAX_CASCADES);
            ImGui::SliderInt("Shadow Map Size", &m_config.shadow_csm_sm_size, 512, 4096);
            ImGui::Checkbox("Visualize CSM Cascade", &m_config.shadow_csm_visualize_cascade);
            ImGui::Checkbox("Enable CSM Blend", &m_config.shadow_csm_blend_option);
            if (m_config.shadow_csm_blend_option) {
                ImGui::SliderFloat("Blend Percentage", &m_config.shadow_csm_blend_percentage, 0, 1);
            }
        };

        auto csm_shadow_cache_param = [&]() {
            ImGui::Separator();
            ImGui::Checkbox("Enable Shadow Cache", &m_config.shadow_cache_enabled);
            ImGui::TextWrapped(
                "First N cascades always refresh. Later cascades may reuse cached shadow maps when camera "
                "motion stays below threshold."
            );
            ImGui::SliderInt(
                "Disable Cache For First N Cascades",
                &m_config.shadow_cache_disable_first_n_cascades,
                0,
                m_config.shadow_csm_num_of_cascades
            );
            m_config.shadow_cache_disable_first_n_cascades =
                Max(0,
                    Min(m_config.shadow_cache_disable_first_n_cascades, m_config.shadow_csm_num_of_cascades));
            for (int i = 0; i < m_config.shadow_csm_num_of_cascades; i++) {
                ImGui::SliderFloat(
                    std::format("Cascade {} Cache Move Threshold (Texels)", i).c_str(),
                    &m_config.shadow_cache_camera_move_threshold_in_texels[i],
                    0.0f,
                    128.0f
                );
            }
        };

        if (m_config.shadow_map_mode == EShadowMapMode::POINT_CUBE) { // Point Cube
            ImGui::SliderInt("Shadow Map Size", &m_config.shadow_csm_sm_size, 512, 4096);
        } else if (m_config.shadow_map_mode == EShadowMapMode::CSM) { // CSM
            csm_common_param();
            float mx = 0.0f;
            for (int i = 0; i < m_config.shadow_csm_num_of_cascades; i++) {
                ImGui::SliderFloat(
                    std::format("{}-th CSM Cover Ratio", i).c_str(),
                    &m_config.shadow_csm_cover_ratio_of_camera[i],
                    0.0f,
                    (i < m_config.shadow_csm_num_of_cascades - 1) ? 0.2f : 1.0f
                );
                m_config.shadow_csm_cover_ratio_of_camera[i] =
                    Max(m_config.shadow_csm_cover_ratio_of_camera[i], mx);
                mx = m_config.shadow_csm_cover_ratio_of_camera[i];
            }
            csm_shadow_cache_param();
        } else if (m_config.shadow_map_mode == EShadowMapMode::CSM_AUTO) { // CSM_Auto
            csm_common_param();
            ImGui::SliderFloat("Lerp Factor", &m_config.shadow_csm_lerp_factor, 0, 1);
            csm_shadow_cache_param();
        }

        ImGui::TreePop();
    }

    // MARK: AO
    if (ImGui::TreeNode(
            "Ambient Occlusion", "Ambient Occlusion: [%s]", s_ao_mode_name_map.at(m_config.ao_mode).c_str()
        )) {

        assert(s_ao_mode_name_map.size() == static_cast<uint32>(EAoMode::NUM));
        for (uint i = 0; i < s_ao_mode_name_map.size(); i++) {
            EAoMode cur_enum = static_cast<EAoMode>(i);
            if (ImGui::Selectable(s_ao_mode_name_map.at(cur_enum).c_str(), m_config.ao_mode == cur_enum)) {
                m_config.ao_mode = cur_enum;
            }
            draw_border();
        }

        ImGui::Separator();
        ImGui::Checkbox("Half Resolution AO", &m_config.ao_half_resolution);

        if (m_config.ao_mode == EAoMode::SSAO || m_config.ao_mode == EAoMode::SSAO_AO_ONLY) {
            ImGui::SliderFloat("Intensity", &m_config.ssao_intensity, 0.0f, 2.0f);
            ImGui::SliderFloat("Ray Trace Radius", &m_config.ssao_max_distance, 0.0f, 5.0f);
            ImGui::SliderInt("Samples Per Pixel", &m_config.ssao_spp, 1, 32);
            ImGui::SliderInt("Sample Radius", &m_config.ssao_sample_radius, 1, 32);

        } else if (m_config.ao_mode == EAoMode::RTAO || m_config.ao_mode == EAoMode::RTAO_AO_ONLY) {
            ImGui::SliderFloat("Intensity", &m_config.rtao_intensity, 0.0f, 1.0f);
            ImGui::SliderFloat("Ray Trace Radius", &m_config.rtao_ray_trace_distance, 0.0f, 20.0f);
            ImGui::SliderInt("Samples Per Pixel", &m_config.rtao_spp, 1, 32);

            ImGui::Text("RTAO Sample Mode:");
            assert(s_rtao_sample_mode_name_map.size() == static_cast<uint32>(ERtaoSampleMode::NUM));
            for (uint i = 0; i < s_rtao_sample_mode_name_map.size(); i++) {
                ERtaoSampleMode cur_enum = static_cast<ERtaoSampleMode>(i);
                if (ImGui::Selectable(
                        s_rtao_sample_mode_name_map.at(cur_enum).c_str(),
                        m_config.rtao_sample_mode == cur_enum
                    )) {
                    m_config.rtao_sample_mode = cur_enum;
                }
                draw_border();
            }

            ImGui::Separator();

            ImGui::Checkbox("Enable RTAO TAA Denoiser", &m_config.rtao_denoiser_enable);

            // Denoiser 启用后才能启用 Reprojection
            if (m_config.rtao_denoiser_enable) {
                ImGui::SliderFloat(
                    "Denoiser History Ratio", &m_config.rtao_denoiser_history_ratio, 0.0f, 1.0f
                );

                ImGui::Checkbox("Enable RTAO Reprojection", &m_config.rtao_denoiser_reprojection_enable);
            } else {
                // m_config.rtao_denoiser_reprojection_enable = false;
            }

            // 启用 Reprojection 后才能启用 Validation
            if (m_config.rtao_denoiser_reprojection_enable) {
                ImGui::Checkbox("Enable RTAO Validation", &m_config.rtao_denoiser_validation_enable);
            } else {
                // m_config.rtao_denoiser_validation_enable = false;
            }

            // 启用 Validation 后的额外选项
            if (m_config.rtao_denoiser_validation_enable) {
                ImGui::SliderFloat(
                    "Validation Depth Threshold", &m_config.rtao_denoiser_valid_depth_threshold, 0.0f, 0.1f
                );
                ImGui::SliderFloat(
                    "Validation Normal Threshold", &m_config.rtao_denoiser_valid_normal_threshold, 0.0f, 1.0f
                );
            }

        } else if (m_config.ao_mode == EAoMode::SSDO || m_config.ao_mode == EAoMode::SSDO_AO_ONLY) {
            ImGui::SliderFloat("Intensity", &m_config.ssao_intensity, 0.0f, 2.0f);
            ImGui::SliderFloat("Indirect Intensity", &m_config.ssdo_indirect_intensity, 0.0f, 2.0f);
            ImGui::SliderFloat("Ray Trace Radius", &m_config.ssdo_max_distance, 0.0f, 20.0f);
            ImGui::SliderInt("Samples Per Pixel", &m_config.ssao_spp, 1, 16);
            ImGui::SliderFloat("Sample Radius", &m_config.ssdo_sample_radius, 0.0f, 5.0f);
            ImGui::SliderFloat("Depth Bias", &m_config.ssdo_depth_bias, 0.0f, 0.1f);
        }

        ImGui::TreePop();
    }

    // MARK: SSR
    if (ImGui::TreeNode("SSR", "SSR: [%s]", (m_config.ssr_is_ssr_enabled == 1 ? "Enable" : "Disable"))) {
        if (ImGui::Selectable("Enable", m_config.ssr_is_ssr_enabled == 1)) {
            m_config.ssr_is_ssr_enabled = 1;
        }
        draw_border();
        if (ImGui::Selectable("Disable", m_config.ssr_is_ssr_enabled == 0)) {
            m_config.ssr_is_ssr_enabled = 0;
        }
        draw_border();

        if (m_config.ssr_is_ssr_enabled == 1) {
            ImGui::Checkbox("Enable Jitter", &m_config.ssr_is_enable_jitter);
            ImGui::Checkbox("Force Ground Enable SSR", &m_config.ssr_is_force_ground_enable_ssr);
            ImGui::SliderInt("Sample Count", &m_config.ssr_sample_count, 1, 64);
            ImGui::SliderFloat("Step Base", &m_config.ssr_step_base, 0.0f, 0.1f);
            ImGui::SliderFloat("Roughness Threshold", &m_config.ssr_roughness_threshold, 0.0f, 1.0f);
            ImGui::SliderFloat("Metallic Threshold", &m_config.ssr_metallic_threshold, 0.0f, 1.0f);
        }

        ImGui::TreePop();
    }

    // MARK: Skybox
    if (ImGui::TreeNode("Skybox")) {

        ImGui::Checkbox("Enable Exposure Correction", &m_config.skybox_exposure_correct_enabled);
        ImGui::SliderFloat(
            "Exp.Cect. Factor (log10)", &m_config.skybox_exposure_correct_factor_log10, -3.0f, 1.0f
        );

        ImGui::TreePop();
    }

    // MARK: Denoiser
    if (ImGui::TreeNode(
            "Denoiser",
            "Denoiser: [%s]",
            s_denoiser_mode_name_map.at(static_cast<EDenoiserMode>(m_config.denoiser_mode)).c_str()
        )) {

        assert(s_denoiser_mode_name_map.size() == static_cast<uint32>(EDenoiserMode::NUM));
        for (uint i = 0; i < s_denoiser_mode_name_map.size(); i++) {
            EDenoiserMode cur_enum = static_cast<EDenoiserMode>(i);
            if (ImGui::Selectable(
                    s_denoiser_mode_name_map.at(cur_enum).c_str(), m_config.denoiser_mode == cur_enum
                )) {
                m_config.denoiser_mode = cur_enum;
            }
            draw_border();
        }

        ImGui::Separator();

        ImGui::SliderInt("双边滤波 Radius", &m_config.denoiser_bfd_kernel_radius, 1, 10);
        ImGui::SliderFloat(
            "双边滤波 SpatialSigma^2", &m_config.denoiser_bfd_spatial_sigma_square, 1.0f, 150.0f
        );
        ImGui::SliderFloat("双边滤波 RangeSigma^2", &m_config.denoiser_bfd_range_sigma_square, 0.001f, 0.05f);

        ImGui::TreePop();
    }

#if WITH_CUDA
    // MARK: AI - Upsample
    if (ImGui::TreeNode(
            "Upsample",
            "Upsample: [%s]",
            s_upsample_mode_name_array[static_cast<uint32>(m_config.upsample_mode)].c_str()
        )) {

        ImGui::Text("目前需要在RasterTextures.h中编译期启用");

        for (uint i = 0; i < s_upsample_mode_name_array.size(); i++) {
            if (ImGui::Selectable(
                    s_upsample_mode_name_array[i].c_str(),
                    m_config.upsample_mode == static_cast<EUpsampleMode>(i)
                )) {
                m_config.upsample_mode = static_cast<EUpsampleMode>(i);
            }
            draw_border();
        }

        if (m_config.upsample_mode == EUpsampleMode::BILINEAR) {
            ImGui::SliderInt("Input Size", &m_config.inSize_x, 0, 1500);
            ImGui::SliderInt("Output Size X", &m_config.outSize_x, 0, 3000);
            ImGui::SliderInt("Output Size Y", &m_config.outSize_y, 0, 3000);
        }

        ImGui::TreePop();
    }

    // MARK: AI - CUDA
    if (ImGui::TreeNode("CUDA", "CUDA: [%s]", (m_config.ai_is_cuda_enabled == 1 ? "Enable" : "Disable"))) {
        if (ImGui::Selectable("Enable", m_config.ai_is_cuda_enabled == 1)) {
            m_config.ai_is_cuda_enabled = 1;
            // 自动选择out_final_output
            for (uint i = 0; i < s_ai_trt_visualize_buffer_array.size(); i++) {
                if (strcmp(s_ai_trt_visualize_buffer_array[i].c_str(), "Engine2 out_final_output") == 0) {
                    m_config.ai_trt_visualize_buffer_idx = i;
                }
            }
            // 自动启用RTAO
            if (m_config.ao_mode != EAoMode::RTAO) {
                m_config.ao_mode = EAoMode::RTAO;
                LOG_INFO("Ambient Occlusion Mode switched to RTAO automatically.");
            }
        }
        draw_border();
        if (ImGui::Selectable("Disable", m_config.ai_is_cuda_enabled == 0)) {
            m_config.ai_is_cuda_enabled = 0;
        }
        draw_border();

        // 这里这个选项，是为了修复ONNX网络无法处理HDR的问题
        // 此处使用了一个非常简单的Reinhard ToneMapping，没有自动曝光。所以明亮处会过曝
        ImGui::Checkbox("Force LDR Input & Output", &m_config.ai_trt_force_ldr);

        if (m_config.ai_is_cuda_enabled == 1) {
            for (uint i = 0; i < s_ai_trt_visualize_buffer_array.size(); i++) {
                if (ImGui::Selectable(
                        s_ai_trt_visualize_buffer_array[i].c_str(), m_config.ai_trt_visualize_buffer_idx == i
                    )) {
                    m_config.ai_trt_visualize_buffer_idx = i;
                    m_config.ai_trt_visualize_buffer     = s_ai_trt_visualize_buffer_array[i];
                }
                draw_border();
            }
        }

        ImGui::TreePop();
    }
#endif

    // MARK: Anti-Aliasing
    if (ImGui::TreeNode(
            "Anti-Aliasing", "Anti-Aliasing: [%s]", s_aa_mode_name_map.at(m_config.aa_mode).c_str()
        )) {

        assert(s_aa_mode_name_map.size() == static_cast<uint32>(EAaMode::NUM));
        for (uint i = 0; i < s_aa_mode_name_map.size(); i++) {
            EAaMode cur_enum = static_cast<EAaMode>(i);
            if (ImGui::Selectable(s_aa_mode_name_map.at(cur_enum).c_str(), m_config.aa_mode == cur_enum)) {
                m_config.aa_mode = cur_enum;
            }
            draw_border();
        }
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Debug")) {
        ImGui::SliderFloat("Debug Param", &m_config.debug_param, 0.0f, 1.0f);
        ImGui::Separator();
        ImGui::Checkbox("Enable FPS Limit", &m_config.debug_fps_limit_enable);
        if (m_config.debug_fps_limit_enable) {
            ImGui::SliderFloat("FPS Limit", &m_config.debug_fps_limit, 0.5f, 240.0f);
        }
        ImGui::TreePop();
    }

    ImGui::TreePop();
}

Render::TextureView RasterUI::GetSelectedFrameBuffer() const {
    return m_frame_buffer_and_name_array[m_config.selected_frame_buffer_index];
}

void RasterUI::RegisterFrameBuffers(const Array<Render::TextureView>& frame_buffer_and_name_array) {
    m_frame_buffer_and_name_array = frame_buffer_and_name_array;

    static bool b_first_load = true;
    if (b_first_load) {
        b_first_load = false;

        m_config.selected_frame_buffer_index = GetDefaultSelectedFrameBufferIndex();
    }

    assert(
        m_config.selected_frame_buffer_index < m_frame_buffer_and_name_array.size() &&
        "Invalid default selected frame buffer index"
    );
}

uint RasterUI::GetDefaultSelectedFrameBufferIndex() const {
    const std::string default_selected_frame_buffer_name = "tonemapping_output";

    for (uint i = 0; i < m_frame_buffer_and_name_array.size(); ++i) {
        if (m_frame_buffer_and_name_array[i].GetTexture()->GetName() == default_selected_frame_buffer_name) {
            return i;
        }
    }

    assert(false && "Invalid default selected frame buffer index");
    return uint(0);
}

} // namespace Moer