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

        static bool               opt_fullscreen  = true;
        static bool               opt_padding     = true;
        static ImGuiDockNodeFlags dockspace_flags = ImGuiDockNodeFlags_None;

        // We are using the ImGuiWindowFlags_NoDocking flag to make the parent window not dockable into,
        // because it would be confusing to have two docking targets within each others.
        ImGuiWindowFlags window_flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking;
        if (opt_fullscreen) {
            const ImGuiViewport* viewport = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(viewport->WorkPos);
            ImGui::SetNextWindowSize(viewport->WorkSize);
            ImGui::SetNextWindowViewport(viewport->ID);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
            window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
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
        ImGui::Begin("Editor Menu", _b_show, window_flags);
        if (!opt_padding)
            ImGui::PopStyleVar();

        if (opt_fullscreen)
            ImGui::PopStyleVar(2);

        // Submit the DockSpace
        ImGuiIO& io = ImGui::GetIO();
        // io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        if (io.ConfigFlags & ImGuiConfigFlags_DockingEnable) {
            ImGuiID dockspace_id = ImGui::GetID("Docking Main");
            ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), dockspace_flags);
        }

        // ImGuiDockNodeFlags dock_flags = ImGuiDockNodeFlags_DockSpace;

        // ImGuiWindowFlags window_flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoTitleBar |
        //                                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
        //                                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground |
        //                                 ImGuiConfigFlags_NoMouseCursorChange | ImGuiWindowFlags_NoBringToFrontOnFocus;

        // const ImGuiViewport* main_viewport = ImGui::GetMainViewport();
        // ImGui::SetNextWindowPos(main_viewport->WorkPos, ImGuiCond_Always);
        // int width, height;
        // WindowContext::GetWindowSize(WindowContext::GetMainWindow(), &width, &height);
        // ImGui::SetNextWindowSize({(float)width, (float)height}, ImGuiCond_Always);
        // ImGui::SetNextWindowViewport(main_viewport->ID);
        // ImGui::Begin("Editor Menu", _b_show, window_flags);

        // ImGuiID main_docking_id = ImGui::GetID("Docking Main");
        // float   menu_height     = 18.f;
        // if (ImGui::DockBuilderGetNode(main_docking_id) == nullptr) {
        //     ImGui::DockBuilderRemoveNode(main_docking_id);
        //     ImGui::DockBuilderAddNode(main_docking_id, dock_flags);

        //     ImGui::DockBuilderSetNodePos(main_docking_id,
        //                                  ImVec2(main_viewport->WorkPos.x, main_viewport->WorkPos.y + menu_height));
        //     ImGui::DockBuilderSetNodeSize(main_docking_id,
        //                                   ImVec2((float)width, (float)height - menu_height));

        //     ImGuiID center = main_docking_id;
        //     ImGuiID main_window;
        //     ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.2f, nullptr, &main_window);
        //     ImGui::DockBuilderDockWindow("Moer Engine", main_window);
        //     ImGui::DockBuilderDockWindow("Inspector", right);

        //     ImGui::DockBuilderFinish(main_docking_id);
        // }
        // ImGui::DockSpace(main_docking_id);
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
        ImGui::ShowStyleEditor();
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