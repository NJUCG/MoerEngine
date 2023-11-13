#include "EditorUI.h"
#include "math/Base.h"
#include "ui/UIBase.h"

#include <imgui.h>
#include <imgui_internal.h>
#include "window/WindowContext.h"
namespace Moer {
    void EditorUI::Init(const UICreateInfo& info) {
    }

    void EditorUI::Tick() {
        ShowEditorMenu(&m_b_show_editor_menu);
        ShowMainWindow(&m_b_show_main_window);
        ShowInspectorWindow(&m_b_show_inspector_window);
    }
    EditorUI::~EditorUI() {
    }

    void EditorUI::ShowEditorMenu(bool* _b_show) {
        ImGuiDockNodeFlags dock_flags = ImGuiDockNodeFlags_DockSpace;

        ImGuiWindowFlags window_flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoTitleBar |
                                        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground |
                                        ImGuiConfigFlags_NoMouseCursorChange | ImGuiWindowFlags_NoBringToFrontOnFocus;

        const ImGuiViewport* main_viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(main_viewport->WorkPos, ImGuiCond_Always);
        int width, height;
        WindowContext::GetWindowSize(WindowContext::GetMainWindow(), &width, &height);
        ImGui::SetNextWindowSize({(float)width, (float)height}, ImGuiCond_Always);
        ImGui::SetNextWindowViewport(main_viewport->ID);
        ImGui::Begin("Editor Menu", _b_show, window_flags);

        ImGuiID main_docking_id = ImGui::GetID("Docking Main");
        float   menu_height     = 18.f;
        if (ImGui::DockBuilderGetNode(main_docking_id) == nullptr) {
            ImGui::DockBuilderRemoveNode(main_docking_id);
            ImGui::DockBuilderAddNode(main_docking_id, dock_flags);

            ImGui::DockBuilderSetNodePos(main_docking_id,
                                         ImVec2(main_viewport->WorkPos.x, main_viewport->WorkPos.y + menu_height));
            ImGui::DockBuilderSetNodeSize(main_docking_id,
                                          ImVec2((float)width, (float)height - menu_height));

            ImGuiID center = main_docking_id;
            ImGuiID main_window;
            ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.2f, nullptr, &main_window);
            ImGui::DockBuilderDockWindow("Moer Engine", main_window);
            ImGui::DockBuilderDockWindow("Inspector", right);

            ImGui::DockBuilderFinish(main_docking_id);
        }
        ImGui::DockSpace(main_docking_id);
        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu("Menu")) {
                if (ImGui::MenuItem("Reload Current Level")) {
                }
                if (ImGui::MenuItem("Save Current Level")) {
                }
                if (ImGui::MenuItem("Exit")) {
                    exit(0);
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Window")) {

                ImGui::MenuItem("Moer Engine", nullptr, &m_b_show_main_window);
                ImGui::MenuItem("Inspector", nullptr, &m_b_show_inspector_window);
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }
        ImGui::End();
    }

    void EditorUI::ShowMainWindow(bool* b_show) {
        ImGuiIO&         io           = ImGui::GetIO();
        ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_MenuBar;

        const ImGuiViewport* main_viewport = ImGui::GetMainViewport();

        if (!*b_show)
            return;

        if (!ImGui::Begin("Moer Engine", b_show, window_flags)) {
            ImGui::End();
            return;
        }

        Moer::Vector2f render_target_window_pos  = {0.0f, 0.0f};
        Moer::Vector2f render_target_window_size = {0.0f, 0.0f};

        auto menu_bar_rect = ImGui::GetCurrentWindow()->MenuBarRect();

        render_target_window_pos.x  = ImGui::GetWindowPos().x;
        render_target_window_pos.y  = menu_bar_rect.Max.y;
        render_target_window_size.x = ImGui::GetWindowSize().x;
        render_target_window_size.y = (ImGui::GetWindowSize().y + ImGui::GetWindowPos().y) - menu_bar_rect.Max.y;// coord of right bottom point of full window minus coord of right bottom point of menu bar window.

        //set viewport size(image size) of main viewport
        // ImGui::Image(ImTextureID user_texture_id, render_target_window_size);

        ImGui::End();
    }

    void EditorUI::ShowInspectorWindow(bool* b_show) {
        ImGuiWindowFlags window_flags = ImGuiWindowFlags_None;

        const ImGuiViewport* main_viewport = ImGui::GetMainViewport();

        if (!*b_show)
            return;

        if (!ImGui::Begin("Inspector", b_show, window_flags)) {
            ImGui::End();
            return;
        }
        ImGui::End();
    }
}// namespace Moer