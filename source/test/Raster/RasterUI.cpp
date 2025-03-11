#include "RasterUI.h"

#include "imgui.h"
#include "imgui_internal.h"

namespace Moer::Render {

    // TODO: merge common code into a base class

    RasterUI::RasterUI(UIRenderer& _renderer) : m_ui_renderer(_renderer) {}

    void RasterUI::TickUI() {

        static bool               opt_fullscreen  = true;
        static bool               opt_padding     = false;
        static ImGuiDockNodeFlags dockspace_flags = ImGuiDockNodeFlags_None;
        // ImGui::DockSpaceOverViewport(ImGui::GetMainViewport());
        // We are using the ImGuiWindowFlags_NoDocking flag to make the parent window not dockable into,
        // because it would be confusing to have two docking targets within each others.
        ImGuiWindowFlags window_flags = ImGuiWindowFlags_MenuBar;
        if (opt_fullscreen) {
            const ImGuiViewport* viewport = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(viewport->WorkPos);
            ImGui::SetNextWindowSize(viewport->WorkSize);
            ImGui::SetNextWindowViewport(viewport->ID);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
            window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
            window_flags |= ImGuiWindowFlags_NoBackground;
        } else {
            dockspace_flags &= ~ImGuiDockNodeFlags_PassthruCentralNode;
        }

        // When using ImGuiDockNodeFlags_PassthruCentralNode, DockSpace() will render our background
        // and handle the pass-thru hole, so we ask Begin() to not render a background.
        if (dockspace_flags & ImGuiDockNodeFlags_PassthruCentralNode)
            window_flags |= ImGuiWindowFlags_NoBackground;

        // Important: note that we proceed even if Begin() returns false (aka window is collapsed).
        // This is because we want to keep our DockSpace() active. If a DockSpace() is inactive,
        // all active windows docked into it will lose their parent and become undocked.
        // We cannot preserve the docking relationship between an active window and an inactive docking, otherwise
        // any change of dockspace/settings would lead to windows being stuck in limbo and never being visible.
        if (!opt_padding) ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::Begin("Editor Menu", &m_b_show, window_flags);
        if (!opt_padding) ImGui::PopStyleVar();

        if (opt_fullscreen) ImGui::PopStyleVar(2);

        // Submit the DockSpace
        ImGuiIO& io = ImGui::GetIO();
        if (io.ConfigFlags & ImGuiConfigFlags_DockingEnable) {
            ImGuiID dockspace_id = ImGui::GetID("Docking Main");
            ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), dockspace_flags);
        }
        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu("Menu")) {
                // if (ImGui::MenuItem("Reload Current Level")) {
                // }
                // if (ImGui::MenuItem("Save Current Level")) {
                // }
                if (ImGui::MenuItem("Exit")) { exit(0); }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Window")) {

                ImGui::MenuItem("Scene Color", nullptr, &m_b_show_scene_color);
                ImGui::MenuItem("Configs", nullptr, &m_b_show_config);
                // ImGui::MenuItem("Inspector", nullptr, &m_m_b_show_inspector_window);
                // ImGui::MenuItem("Demo", nullptr, &m_b_show_demo);
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }
        ImGui::End();

        InitUIStyle();
        ShowSceneColor();
        ShowConfig();
    }

    bool RasterUI::IsSeperateWindow() const {
        auto* current_window = ImGui::FindWindowByName("Scene Color");
        return current_window->ParentWindow == nullptr;
    }

    TextureView RasterUI::GetWindowFrameBuffer() {
        auto* current_window = ImGui::FindWindowByName("Scene Color");
        if (current_window->ParentWindow == nullptr) {
            return m_ui_renderer.GetWindowFrameBuffer(current_window->Viewport);
        }
        return TextureView();
    }

    void RasterUI::RegisterFrameBuffers(const Array<TextureView>& frame_buffer_and_name_array) {
        m_frame_buffer_and_name_array        = frame_buffer_and_name_array;
        m_config.selected_frame_buffer_index = GetDefaultSelectedFrameBufferIndex();
        assert(
            m_config.selected_frame_buffer_index < m_frame_buffer_and_name_array.size() &&
            "Invalid default selected frame buffer index"
        );
    }

    void RasterUI::InitUIStyle() {
        ImGuiStyle& style   = ImGui::GetStyle();
        style.ItemSpacing.y = 7.f; // default is 4.f
    }

    void RasterUI::ShowSceneColor() {
        ImGuiIO&         io           = ImGui::GetIO();
        ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_MenuBar;

        const ImGuiViewport* main_viewport = ImGui::GetMainViewport();
        if (!m_b_show_scene_color) { return; }
        if (!ImGui::Begin("Scene Color", &m_b_show_scene_color, window_flags)) {

            ImGui::End();
            return;
        }
        float2 scene_size = {0, 0};

        static float2 xy_ratio = {16, 9};
        // auto          menu_rect = ImGui::GetCurrentWindow()->MenuBarRect();

        auto* current_window      = ImGui::FindWindowByName("Scene Color");
        bool  m_b_separate_window = current_window->ParentWindow == nullptr;
        auto  menu_rect           = current_window->MenuBarRect();

        scene_size.x = current_window->Size.x;
        // why use this formula?
        // scene_size.y = current_window->Size.y + current_window->Pos.y - menu_rect.Max.y;
        scene_size.y = current_window->Size.y;

        auto   window_rect = current_window->Rect(); // this is main window rect
        ImRect parent_rect{};

        if (m_b_separate_window) {
            parent_rect = {
                current_window->Pos.x,
                current_window->Pos.y,
                current_window->Pos.x + current_window->Size.x,
                current_window->Pos.y + current_window->Size.y
            };
        } else {
            parent_rect = current_window->ParentWindow->Rect();
        }
        float2 local_pos = {window_rect.Min.x - parent_rect.Min.x, menu_rect.Max.y - parent_rect.Min.y};

        m_scene_color_resolution = {scene_size.x, scene_size.y};
        m_scene_color_pos        = {local_pos.x, local_pos.y};

        ImGui::End();
    }

    void RasterUI::ShowConfig() {
        ImGuiIO&         io           = ImGui::GetIO();
        ImGuiWindowFlags window_flags = ImGuiWindowFlags_None;

        if (!m_b_show_config) { return; }
        if (!ImGui::Begin("Configs", &m_b_show_config, window_flags)) {
            ImGui::End();
            return;
        }

        ImGui::Text("FPS: %.1f", io.Framerate);

        auto draw_border = [&]() {
            // 获取选项的矩形区域
            ImVec2 min = ImGui::GetItemRectMin();
            ImVec2 max = ImGui::GetItemRectMax();
            // 绘制边框
            ImGui::GetWindowDrawList()->AddRect(min, max, IM_COL32(255, 255, 255, 255));
        };

        m_config.b_reset = false;
        if (ImGui::Button("Reset")) { m_config.b_reset = true; }

        if (ImGui::TreeNode(
                "Output Frame Buffer",
                "Output: [%s]",
                m_frame_buffer_and_name_array[m_config.selected_frame_buffer_index]
                    .GetTexture()
                    ->GetName()
                    .data()
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

        if (ImGui::TreeNode("AO Mode", "AO Mode: [%s]", k_ao_mode_name_array[m_config.ao_mode].c_str())) {
            for (uint i = 0; i < k_ao_mode_name_array.size(); i++) {
                if (ImGui::Selectable(k_ao_mode_name_array[i].c_str(), m_config.ao_mode == i) && i != 3 &&
                    i != 4 // SSDO is not implemented yet
                ) {
                    m_config.ao_mode = i;
                }
                draw_border();
            }

            ImGui::SliderFloat("Intensity", &m_config.ssao_intensity, 0.0f, 2.0f);
            ImGui::SliderInt("Sample Count", &m_config.ssao_sample_count, 1, 16);
            ImGui::SliderInt("Sample Radius", &m_config.ssao_radius, 1, 8);
            ImGui::SliderFloat("Max Distance", &m_config.ssao_max_distance, 0.0f, 2.0f);

            ImGui::TreePop();
        }

        if (ImGui::TreeNode(
                "SSR Mode", "SSR: [%s]", (m_config.ssr_is_enable_ssr == 1 ? "Enable" : "Disable")
            )) {
            if (ImGui::Selectable("Enable", m_config.ssr_is_enable_ssr == 1)) {
                m_config.ssr_is_enable_ssr = 1;
            }
            draw_border();
            if (ImGui::Selectable("Disable", m_config.ssr_is_enable_ssr == 0)) {
                m_config.ssr_is_enable_ssr = 0;
            }
            draw_border();

            if (m_config.ssr_is_enable_ssr == 1) {
                ImGui::Checkbox("Enable Jitter", &m_config.ssr_is_enable_jitter);
                ImGui::Checkbox("Force Ground Enable SSR", &m_config.ssr_is_force_ground_enable_ssr);
                ImGui::SliderInt("Sample Count", &m_config.ssr_sample_count, 1, 64);
                ImGui::SliderFloat("Step Base", &m_config.ssr_step_base, 0.0f, 0.1f);
                ImGui::SliderFloat("Roughness Threshold", &m_config.ssr_roughness_threshold, 0.0f, 1.0f);
                ImGui::SliderFloat("Metallic Threshold", &m_config.ssr_metallic_threshold, 0.0f, 1.0f);
            }

            ImGui::TreePop();
        }

        if (ImGui::TreeNode(
                "AA Mode", "Anti-Aliasing Mode: [%s]", k_aa_mode_name_array[m_config.aa_mode].c_str()
            )) {
            for (uint i = 0; i < k_aa_mode_name_array.size(); i++) {
                if (ImGui::Selectable(k_aa_mode_name_array[i].c_str(), m_config.aa_mode == i)) {
                    m_config.aa_mode = i;
                }
                draw_border();
            }
            ImGui::TreePop();
        }

        ImGui::End();
    }

    uint RasterUI::GetDefaultSelectedFrameBufferIndex() const {
        const std::string default_selected_frame_buffer_name = "aa_output";

        for (uint i = 0; i < m_frame_buffer_and_name_array.size(); ++i) {
            if (m_frame_buffer_and_name_array[i].GetTexture()->GetName() ==
                default_selected_frame_buffer_name) {
                return i;
            }
        }

        assert(false && "Invalid default selected frame buffer index");
        return uint(0);
    }
} // namespace Moer::Render