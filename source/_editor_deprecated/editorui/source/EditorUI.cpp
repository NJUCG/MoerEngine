#include "ui/EditorUI.h"
#include "math/Base.h"
#include "ui/UIBase.h"
#define ImTextureID uint64_t
#include "rhi/RHIResource.h"
#include "window/WindowContext.h"
#include <imgui.h>
#include <imgui_internal.h>

#include "MainWindow.h"

namespace Moer {
MainWindow  g_main_window;
static bool show_demo_window = true;

void EditorUI::Init(const UICreateInfo& info) {}

void EditorUI::Tick() {
    ShowEditorMenu(&m_b_show_editor_menu);
    g_main_window.Show();
    ShowInspectorWindow(&m_b_show_inspector_window);
    // ImGui::ShowDemoWindow(&show_demo_window);
}
EditorUI::~EditorUI() {}

void EditorUI::ShowEditorMenu(bool* _b_show) {

    static bool               opt_fullscreen  = true;
    static bool               opt_padding     = false;
    static ImGuiDockNodeFlags dockspace_flags = ImGuiDockNodeFlags_None;

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

            ImGui::MenuItem("Moer Engine", nullptr, g_main_window.ShowWindow());
            // ImGui::MenuItem("Inspector", nullptr, &m_b_show_inspector_window);
            // ImGui::MenuItem("Demo", nullptr, &show_demo_window);
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
    ImGui::End();
}

void EditorUI::ShowMainWindow(bool* b_show) {
    ImGuiIO&         io           = ImGui::GetIO();
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoTitleBar;

    const ImGuiViewport* main_viewport = ImGui::GetMainViewport();

    if (!*b_show)
        return;

    if (!ImGui::Begin("Moer Engine", b_show, window_flags)) {
        ImGui::End();
        return;
    }

    Moer::Vector2f render_target_window_pos  = {0.0f, 0.0f};
    Moer::Vector2f render_target_window_size = {0.0f, 0.0f};

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
} // namespace Moer