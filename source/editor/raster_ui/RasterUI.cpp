#include "RasterUI.h"

#include "config/ConfigManager.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <string_view>

namespace Moer {

RasterUI::RasterUI(RasterConfig& config) : m_config(config) {
    m_config.shadow_map_mode =
        (ConfigManager::GetInstance().GetConfig().engine.render.raster.enable_shadow ?
             m_config.shadow_map_mode :
             0);
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

    if (ImGui::TreeNode(
            "Shading",
            "Shading: [%s]",
            s_shading_mode_name_array[static_cast<size_t>(m_config.shading_mode)].c_str()
        )) {
        assert(s_shading_mode_name_array.size() == static_cast<size_t>(EShadingMode::NUM));
        for (uint i = 0; i < s_shading_mode_name_array.size(); i++) {
            if (ImGui::Selectable(
                    s_shading_mode_name_array[i].c_str(),
                    m_config.shading_mode == static_cast<EShadingMode>(i)
                )) {
                m_config.shading_mode = static_cast<EShadingMode>(i);
            }
            draw_border();
        }

        ImGui::Separator();

        ImGui::Checkbox("Enable Extra Ambient", &m_config.shading_enable_extra_ambient);
        ImGui::SliderFloat("Ambient Intensity", &m_config.shading_extra_ambient_intensity, 0.0f, 1.0f);
        ImGui::SliderFloat3("Ambient Color", (float*)&m_config.shading_extra_ambient_color, 0.0f, 1.0f);

        ImGui::TreePop();
    }

    if (ImGui::TreeNode(
            "Shadow", "Shadow: [%s]", s_shadow_map_mode_name_array[m_config.shadow_map_mode].c_str()
        )) {

        ImGui::Text("Only project 1st Dir.Light");

        for (uint i = 0; i < s_shadow_map_mode_name_array.size(); i++) {
            if (ImGui::Selectable(s_shadow_map_mode_name_array[i].c_str(), m_config.shadow_map_mode == i)) {
                m_config.shadow_map_mode = i;
            }
            draw_border();
        }

        if (m_config.shadow_map_mode == 1) { // CSM
            ImGui::SliderInt("Num of Cascades", &m_config.shadow_csm_num_of_cascades, 1, CSM_MAX_CASCADES);
            ImGui::SliderInt("Shadow Map Size", &m_config.shadow_csm_sm_size, 512, 4096);
            float mx = 0.0f;
            for (int i = 0; i < m_config.shadow_csm_num_of_cascades; i++) {
                ImGui::SliderFloat(
                    std::format("{}-th CSM Cover Ratio", i).c_str(),
                    &m_config.shadow_csm_cover_ratio_of_camera[i],
                    0.0f,
                    1.0f
                );
                m_config.shadow_csm_cover_ratio_of_camera[i] =
                    Max(m_config.shadow_csm_cover_ratio_of_camera[i], mx);
                mx = m_config.shadow_csm_cover_ratio_of_camera[i];
            }
        } else if (m_config.shadow_map_mode == 2) { // VSM
            // TODO
        }

        ImGui::TreePop();
    }

    if (ImGui::TreeNode(
            "Ambient Occlusion",
            "Ambient Occlusion: [%s]",
            s_ao_mode_name_array[static_cast<uint32>(m_config.ao_mode)].c_str()
        )) {

        assert(s_ao_mode_name_array.size() == static_cast<uint32>(EAoMode::NUM));
        for (uint i = 0; i < s_ao_mode_name_array.size(); i++) {
            if (ImGui::Selectable(
                    s_ao_mode_name_array[i].c_str(), m_config.ao_mode == static_cast<EAoMode>(i)
                )) {
                m_config.ao_mode = static_cast<EAoMode>(i);
            }
            draw_border();
        }

        if (m_config.ao_mode == EAoMode::SSAO || m_config.ao_mode == EAoMode::SSAO_AO_ONLY) {
            ImGui::SliderFloat("Intensity", &m_config.ssao_intensity, 0.0f, 2.0f);
            ImGui::SliderFloat("Ray Trace Radius", &m_config.ssao_max_distance, 0.0f, 2.0f);
            ImGui::SliderInt("Samples Per Pixel", &m_config.ssao_spp, 1, 16);
            ImGui::SliderInt("Sample Radius", &m_config.ssao_sample_radius, 1, 8);
        } else if (m_config.ao_mode == EAoMode::RTAO || m_config.ao_mode == EAoMode::RTAO_AO_ONLY) {
            ImGui::SliderFloat("Intensity", &m_config.rtao_intensity, 0.0f, 1.0f);
            ImGui::SliderFloat("Ray Trace Radius", &m_config.rtao_ray_trace_distance, 0.0f, 20.0f);
            ImGui::SliderInt("Samples Per Pixel", &m_config.rtao_spp, 1, 32);
            ImGui::Text("RTAO Sample Mode:");
            assert(s_rtao_sample_mode.size() == static_cast<uint32>(ERtaoSampleMode::NUM));
            for (uint i = 0; i < s_rtao_sample_mode.size(); i++) {
                if (ImGui::Selectable(
                        s_rtao_sample_mode[i].c_str(),
                        m_config.rtao_sample_mode == static_cast<ERtaoSampleMode>(i)
                    )) {
                    m_config.rtao_sample_mode = static_cast<ERtaoSampleMode>(i);
                }
                draw_border();
            }
        }

        ImGui::TreePop();
    }

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

    if(ImGui::TreeNode("Upsample", 
            "Upsample: [%s]", 
             s_upsample_mode_name_array[static_cast<uint32>(m_config.upsample_mode)].c_str())){
        for(uint i = 0; i < s_upsample_mode_name_array.size(); i++){
            if(ImGui::Selectable(
                s_upsample_mode_name_array[i].c_str(), m_config.upsample_mode == static_cast<EUpsampleMode>(i)
            )){
                m_config.upsample_mode = static_cast<EUpsampleMode>(i);
            }
            draw_border();
        }

        if(m_config.upsample_mode == EUpsampleMode::BILINEAR) {
            ImGui::SliderInt("Input Size", &m_config.inSize_x, 0, 1500);
            ImGui::SliderInt("Output Size X", &m_config.outSize_x, 0, 3000);
            ImGui::SliderInt("Output Size Y", &m_config.outSize_y, 0, 3000);
        }

        ImGui::TreePop();
    }

#if WITH_CUDA
    if (ImGui::TreeNode("CUDA", "CUDA: [%s]", (m_config.ai_is_cuda_enabled == 1 ? "Enable" : "Disable"))) {
        if (ImGui::Selectable("Enable", m_config.ai_is_cuda_enabled == 1)) {
            m_config.ai_is_cuda_enabled = 1;
        }
        draw_border();
        if (ImGui::Selectable("Disable", m_config.ai_is_cuda_enabled == 0)) {
            m_config.ai_is_cuda_enabled = 0;
        }
        draw_border();

        if (m_config.ai_is_cuda_enabled == 1) {
            ImGui::SliderFloat("Debug Param", &m_config.ai_cuda_pass_debug_param, 0.0f, 1.0f);

            // ImGui::Checkbox("Enable Jitter", &m_config.ssr_is_enable_jitter);
            // ImGui::Checkbox("Force Ground Enable SSR", &m_config.ssr_is_force_ground_enable_ssr);
            // ImGui::SliderInt("Sample Count", &m_config.ssr_sample_count, 1, 64);
            // ImGui::SliderFloat("Roughness Threshold", &m_config.ssr_roughness_threshold, 0.0f, 1.0f);
            // ImGui::SliderFloat("Metallic Threshold", &m_config.ssr_metallic_threshold, 0.0f, 1.0f);

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

    if (ImGui::TreeNode(
            "Anti-Aliasing",
            "Anti-Aliasing: [%s]",
            s_aa_mode_name_array[static_cast<uint32>(m_config.aa_mode)].c_str()
        )) {

        assert(s_aa_mode_name_array.size() == static_cast<uint32>(EAaMode::NUM));
        for (uint i = 0; i < s_aa_mode_name_array.size(); i++) {
            if (ImGui::Selectable(
                    s_aa_mode_name_array[i].c_str(), m_config.aa_mode == static_cast<EAaMode>(i)
                )) {
                m_config.aa_mode = static_cast<EAaMode>(i);
            }
            draw_border();
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
    const std::string default_selected_frame_buffer_name = "aa_output";

    for (uint i = 0; i < m_frame_buffer_and_name_array.size(); ++i) {
        if (m_frame_buffer_and_name_array[i].GetTexture()->GetName() == default_selected_frame_buffer_name) {
            return i;
        }
    }

    assert(false && "Invalid default selected frame buffer index");
    return uint(0);
}

} // namespace Moer