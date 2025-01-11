#include "RTUI.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "math/Function.h"
#include "shaderheaders/shared/ShaderParameters.h"
namespace Moer::Render {

    static constexpr std::string_view s_final_color_names[] = {
        "SceneColor",
        "Emissive",
        "Diffuse",
        "Specular",
        "Normal",
        "ViewDepth",
        "Depth",
        "Motion",
        "Grid",
        "Material"};
    RTUI::RTUI(UIRenderer& _renderer)
        : ui_renderer(_renderer) {

        final_color_map["SceneColor"] = EFinalColor::EFC_SceneColor;
        final_color_map["Emissive"]   = EFinalColor::EFC_EMISSIVE;
        final_color_map["Diffuse"]    = EFinalColor::EFC_DIFFUSE;
        final_color_map["Specular"]   = EFinalColor::EFC_SPECULAR;
        final_color_map["Normal"]     = EFinalColor::EFC_NORMAL;
        final_color_map["ViewDepth"]  = EFinalColor::EFC_VIEW_DEPTH;
        final_color_map["Depth"]      = EFinalColor::EFC_DEPTH;
        final_color_map["Motion"]     = EFinalColor::EFC_MOTION;
        final_color_map["Grid"]       = EFinalColor::EFC_GRID;
        final_color_map["Material"]   = EFinalColor::EFC_MATERIAL;
    }
    void RTUI::TickUI() {

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
        ImGui::Begin("Editor Menu", &b_show, window_flags);
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

                ImGui::MenuItem("Scene Color", nullptr, &b_show_scene_color);
                ImGui::MenuItem("Configs", nullptr, &b_show_config);
                // ImGui::MenuItem("Inspector", nullptr, &m_b_show_inspector_window);
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }
        ImGui::End();

        ShowSceneColor();
        ShowConfig();
    }

    void RTUI::ShowSceneColor() {
        ImGuiIO&         io           = ImGui::GetIO();
        ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_MenuBar;

        const ImGuiViewport* main_viewport = ImGui::GetMainViewport();
        if (!b_show_scene_color) {
            return;
        }
        if (!ImGui::Begin("Scene Color", &b_show_scene_color, window_flags)) {

            ImGui::End();
            return;
        }
        float2 scene_size = {0, 0};

        static float2 xy_ratio = {16, 9};
        // auto          menu_rect = ImGui::GetCurrentWindow()->MenuBarRect();

        auto* current_window    = ImGui::FindWindowByName("Scene Color");
        bool  b_separate_window = current_window->ParentWindow == nullptr;
        auto  menu_rect         = current_window->MenuBarRect();

        scene_size.x = current_window->Size.x;
        scene_size.y = current_window->Size.y + current_window->Pos.y - menu_rect.Max.y;

        auto   window_rect = current_window->Rect();// this is main window rect
        ImRect parent_rect{};

        if (b_separate_window) {

            parent_rect = {current_window->Pos.x, current_window->Pos.y, current_window->Pos.x + current_window->Size.x, current_window->Pos.y + current_window->Size.y};

            float2 local_pos = {window_rect.Min.x - parent_rect.Min.x, menu_rect.Max.y - parent_rect.Min.y};

            scene_color_resolution = {scene_size.x, scene_size.y};
            scene_color_pos        = {local_pos.x, local_pos.y};
        } else {
            parent_rect = current_window->ParentWindow->Rect();

            float2 local_pos = {window_rect.Min.x - parent_rect.Min.x, menu_rect.Max.y - parent_rect.Min.y};

            scene_color_resolution = {scene_size.x, scene_size.y};
            scene_color_pos        = {local_pos.x, local_pos.y};
        }

        ImGui::End();
    }

    void RTUI::ShowConfig() {
        ImGuiIO&         io           = ImGui::GetIO();
        ImGuiWindowFlags window_flags = ImGuiWindowFlags_None;

        if (!b_show_config) {
            return;
        }
        if (!ImGui::Begin("Configs", &b_show_config, window_flags)) {

            ImGui::End();
            return;
        }

        if (ImGui::TreeNode("Final Color")) {
            for (auto& [name, index] : final_color_map) {
                if (ImGui::Selectable(name.c_str(), config.final_color == index)) {
                    config.final_color = static_cast<EFinalColor>(index);
                }
            }

            ImGui::TreePop();
        }

        config.sun_direction = Normalizef(config.sun_direction);
        ImGui::SliderFloat3("Sun Direction", &config.sun_direction.x, -1.0f, 1.0f);
        ImGui::SliderFloat("Exposure", &config.exposure, 0.0f, 10.0f);
        ImGui::SliderFloat("Sun Angular Diameter", &config.sun_angular_diameter, 0.0f, 1.0f);

        int max_bounce = config.max_bounce;
        ImGui::SliderInt("Max Bounce", &max_bounce, 1, 5);
        config.max_bounce = max_bounce;
        //show fps
        ImGui::Text("FPS: %.1f", io.Framerate);
        ImGui::Text("Frame Time: %.1f ms", 1000.0f / io.Framerate);

        ImGui::End();
    }

    bool RTUI::IsSeperateWindow() const {
        auto* current_window = ImGui::FindWindowByName("Scene Color");

        return current_window->ParentWindow == nullptr;
    }

    TextureView RTUI::GetWindowFrameBuffer() {
        auto* current_window = ImGui::FindWindowByName("Scene Color");
        if (current_window->ParentWindow == nullptr) {
            return ui_renderer.GetWindowFrameBuffer(current_window->Viewport);
        }
        return TextureView();
    }
}// namespace Moer::Render