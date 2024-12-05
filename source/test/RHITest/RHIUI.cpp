#include "RHIUI.h"
#include "imgui.h"
#include "imgui_internal.h"

namespace Moer::Render {

    // TODO: merge common code into a base class

    RHIUI::RHIUI(
        UIRenderer&                                       _renderer,
        const Array<std::pair<TextureView, std::string>>& frame_buffer_and_name_array,
        uint                                              default_selected_frame_buffer_index)
        : m_ui_renderer(_renderer) {

        RegisterFrameBuffers(frame_buffer_and_name_array, default_selected_frame_buffer_index);
    }

    void RHIUI::TickUI() {

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
            window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
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
        if (!opt_padding)
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::Begin("Editor Menu", &m_b_show, window_flags);
        if (!opt_padding)
            ImGui::PopStyleVar();

        if (opt_fullscreen)
            ImGui::PopStyleVar(2);

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
                if (ImGui::MenuItem("Exit")) {
                    exit(0);
                }
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

    void RHIUI::RegisterFrameBuffers(const Array<std::pair<TextureView, std::string>>& frame_buffer_and_name_array, uint default_selected_frame_buffer_index) {
        assert(default_selected_frame_buffer_index < frame_buffer_and_name_array.size() && "Invalid default selected frame buffer index");
        m_frame_buffer_and_name_array        = frame_buffer_and_name_array;
        m_config.selected_frame_buffer_index = default_selected_frame_buffer_index;
    }

    bool RHIUI::IsSeperateWindow() const {
        auto* current_window = ImGui::FindWindowByName("Scene Color");
        return current_window->ParentWindow == nullptr;
    }

    TextureView RHIUI::GetWindowFrameBuffer() {
        auto* current_window = ImGui::FindWindowByName("Scene Color");
        if (current_window->ParentWindow == nullptr) {
            return m_ui_renderer.GetWindowFrameBuffer(current_window->Viewport);
        }
        return TextureView();
    }

    void RHIUI::InitUIStyle() {
        ImGuiStyle& style   = ImGui::GetStyle();
        style.ItemSpacing.y = 7.f;// default is 4.f
    }

    void RHIUI::ShowSceneColor() {
        ImGuiIO&         io           = ImGui::GetIO();
        ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_MenuBar;

        const ImGuiViewport* main_viewport = ImGui::GetMainViewport();
        if (!m_b_show_scene_color) {
            return;
        }
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

        auto   window_rect = current_window->Rect();// this is main window rect
        ImRect parent_rect{};

        if (m_b_separate_window) {
            parent_rect = {current_window->Pos.x, current_window->Pos.y, current_window->Pos.x + current_window->Size.x, current_window->Pos.y + current_window->Size.y};
        } else {
            parent_rect = current_window->ParentWindow->Rect();
        }
        float2 local_pos = {window_rect.Min.x - parent_rect.Min.x, menu_rect.Max.y - parent_rect.Min.y};

        m_scene_color_resolution = {scene_size.x, scene_size.y};
        m_scene_color_pos        = {local_pos.x, local_pos.y};

        ImGui::End();
    }

    void RHIUI::ShowConfig() {
        ImGuiIO&         io           = ImGui::GetIO();
        ImGuiWindowFlags window_flags = ImGuiWindowFlags_None;

        if (!m_b_show_config) {
            return;
        }
        if (!ImGui::Begin("Configs", &m_b_show_config, window_flags)) {
            ImGui::End();
            return;
        }

        ImGui::Text("FPS: %.1f", io.Framerate);

        // ImGui::Dummy(ImVec2(0, 10));

        auto draw_border = [&]() {
            // 获取选项的矩形区域
            ImVec2 min = ImGui::GetItemRectMin();
            ImVec2 max = ImGui::GetItemRectMax();
            // 绘制边框
            ImGui::GetWindowDrawList()->AddRect(min, max, IM_COL32(255, 255, 255, 255));
        };

        if (ImGui::TreeNode("Output Frame Buffer", "Output: [%s]", m_frame_buffer_and_name_array[m_config.selected_frame_buffer_index].second.c_str())) {
            for (uint i = 0; i < m_frame_buffer_and_name_array.size(); i++) {
                if (ImGui::Selectable(
                        m_frame_buffer_and_name_array[i].second.c_str(),
                        m_config.selected_frame_buffer_index == i)) {
                    m_config.selected_frame_buffer_index = i;
                }
                draw_border();
            }
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("AA Mode", "Anti-Aliasing Mode: [%s]", k_aa_mode_name_array[m_config.aa_mode].c_str())) {
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
}// namespace Moer::Render