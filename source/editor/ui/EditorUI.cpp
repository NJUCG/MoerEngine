#include "ui/EditorUI.h"

// Runtime
#include "config/ConfigManager.h"
#include "window/WindowInput.h"

// Editor
#include "EditorUIStyle.h"

// 3rd party (std)
#include <imgui.h>
#include <imgui_internal.h>
#include <nfd.hpp>
#include <string_view>

using namespace Moer::Render;

namespace Moer {

EditorUI::EditorUI(UniquePtr<Render::UIRenderer> renderer, uint2 resolution) :
    m_ui_renderer(std::move(renderer)),
    m_resolution(resolution) {

    // Load Config
    InitFromConfigManager();

    // Init Style
    EditorUIStyle::ApplyDefaultStyle();
}

void EditorUI::InitFromConfigManager() {
    auto config = ConfigManager::GetInstance().GetConfig();

    // render method
    if (config.engine.render.default_render_method == "Raster") {
        m_config.selected_render_method = ERenderMethod::Raster;
    } else if (config.engine.render.default_render_method == "Raytracing") {
        m_config.selected_render_method = ERenderMethod::Raytracing;
    } else {
        LOG_WARNING(
            "Invalid default render method: {}. Use Raster instead.",
            config.engine.render.default_render_method
        );
        m_config.selected_render_method = ERenderMethod::Raster;
    }

    // scene path
    m_config.scene_path = config.engine.scene.scene_path;
}

void EditorUI::TickUI() {
    m_ui_renderer->BeginGUIFrame();

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

    ResetState();
    ShowSceneColor();
    ShowConfig();

    m_ui_renderer->EndGUIFrame();
}

void EditorUI::RenderGUI(Render::CommandList& cmd_list, const Render::TextureView& final_output) {
    m_ui_renderer->RenderGUI(cmd_list, final_output);
}

void EditorUI::PresentWindows() { m_ui_renderer->PresentWindows(); }

bool EditorUI::IsSeperateWindow() const {
    auto* current_window = ImGui::FindWindowByName("Scene Color");
    return current_window->ParentWindow == nullptr;
}

TextureView EditorUI::GetWindowFrameBuffer() {
    auto* current_window = ImGui::FindWindowByName("Scene Color");
    if (current_window->ParentWindow == nullptr) {
        return m_ui_renderer->GetWindowFrameBuffer(current_window->Viewport);
    }
    return TextureView();
}

void EditorUI::ShowSceneColor() {
    ImGuiIO&         io           = ImGui::GetIO();
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_MenuBar;

    const ImGuiViewport* main_viewport = ImGui::GetMainViewport();
    if (!m_b_show_scene_color) { return; }
    if (!ImGui::Begin("Scene Color", &m_b_show_scene_color, window_flags)) {
        // Should not call ImGui::End() here
        return;
    }

    float2 scene_size = {0, 0};

    static float2 xy_ratio = {16, 9};
    // auto          menu_rect = ImGui::GetCurrentWindow()->MenuBarRect();

    auto* current_window      = ImGui::FindWindowByName("Scene Color");
    bool  m_b_separate_window = current_window->ParentWindow == nullptr;
    auto  menu_rect           = current_window->MenuBarRect();
    auto  menu_bar            = current_window->MenuBarHeight();

    scene_size.x = current_window->Size.x;
    scene_size.y = current_window->Size.y + current_window->Pos.y - menu_rect.Max.y; // what is this?

    auto   window_rect = current_window->Rect(); // this is main window rect
    ImRect parent_rect{};

    if (m_b_separate_window) {

        parent_rect = {
            current_window->Pos.x,
            current_window->Pos.y,
            current_window->Pos.x + current_window->Size.x,
            current_window->Pos.y + current_window->Size.y
        };

        float2 local_pos = {window_rect.Min.x - parent_rect.Min.x, menu_rect.Max.y - parent_rect.Min.y};

        m_scene_color_resolution = {scene_size.x, scene_size.y};
        m_scene_color_pos        = {local_pos.x, local_pos.y};
    } else {
        parent_rect = current_window->ParentWindow->Rect();

        float2 local_pos = {
            window_rect.Min.x - parent_rect.Min.x, menu_rect.Max.y + menu_bar - parent_rect.Min.y
        };

        m_scene_color_resolution = {scene_size.x, scene_size.y};
        m_scene_color_pos        = {local_pos.x, local_pos.y};
    }

    // inject. Needs to be refactored (camera control)
    // 只有在Cursor位于SceneColor窗口上时，才可以控制摄像机
    uint2 mouse_pos = uint2(
        ImGui::GetMousePos().x - ImGui::GetWindowPos().x, ImGui::GetMousePos().y - ImGui::GetWindowPos().y
    );
    static uint border = 4;

    WindowInput::Get().is_active = mouse_pos.x > m_scene_color_pos.x + border &&
                                   mouse_pos.x < m_scene_color_pos.x + m_scene_color_resolution.x - border &&
                                   mouse_pos.y > m_scene_color_pos.x + border &&
                                   mouse_pos.y < m_scene_color_pos.y + m_scene_color_resolution.y - border;

    ImGui::End();
}

void EditorUI::ShowConfig() {
    ImGuiIO&         io           = ImGui::GetIO();
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_None;

    if (!m_b_show_config) { return; }
    if (!ImGui::Begin("Configs", &m_b_show_config, window_flags)) { return; }

    ImGui::PushItemWidth(120); // 设置所有组件width为120

    EditorUIStyle::ShowStyleSelector("Style##Default");

    auto last_selected_render_method = m_config.selected_render_method;
    if (ImGui::BeginCombo(
            "Render Method", k_render_method_names[static_cast<uint>(m_config.selected_render_method)].data()
        )) {
        for (int i = 0; i < IM_ARRAYSIZE(k_render_method_names); i++) {
            const bool is_selected = (m_config.selected_render_method == static_cast<ERenderMethod>(i));
            if (ImGui::Selectable(k_render_method_names[i].data(), is_selected)) {
                m_config.selected_render_method = static_cast<ERenderMethod>(i);
            }
            if (is_selected) { ImGui::SetItemDefaultFocus(); }
        }
        ImGui::EndCombo();
    }
    if (last_selected_render_method != m_config.selected_render_method) {
        m_b_need_reload = true;
        SetShowSubUI(false);
    }

    { // Scene Path
        size_t      last_slash = m_config.scene_path.find_last_of("/\\");
        std::string scene_name = (last_slash == std::string::npos) ?
                                     m_config.scene_path :
                                     m_config.scene_path.substr(last_slash + 1);
        if (ImGui::Button("Open Scene")) {
            NFD::UniquePath        selected_path = nullptr;
            Array<nfdfilteritem_t> filters       = {
                {"glTF 2.0", "glb,gltf"},
                {"FBX", "fbx"},
                {"Wavefront", "obj"},
                {"Moer Renderer Scene (WIP)", "json"},
            };
            nfdresult_t result = NFD::OpenDialog(selected_path, filters.data(), filters.size());
            if (result == NFD_OKAY) {
                LOG_INFO("User selected file: {}", selected_path.get());

                // Prepare for reload
                m_b_need_reload     = true;
                m_config.scene_path = selected_path.get();
            } else if (result == NFD_CANCEL) {
                LOG_INFO("User pressed cancel.");
            } else {
                LOG_ERROR("NFD Error: {}", NFD_GetError());
            }
        }
        ImGui::SameLine();
        ImGui::Text("Current: [%s]", scene_name.c_str());
    }

    if (m_b_show_sub_ui) {
        ImGui::Separator();
        switch (m_config.selected_render_method) {
            case ERenderMethod::Raster: m_raster_ui.ShowConfig(); break;
            case ERenderMethod::Raytracing: m_raytracing_ui.ShowConfig(); break;
            default: break;
        }
    }

    ImGui::Separator();

    ImGui::Text("Infos: ");
    ImGui::Text("\tFPS: %.1f", io.Framerate);
    ImGui::Text("\tFrame Time: %.1f ms", 1000.0f / io.Framerate);

    // Custom Func
    for (auto& [name, func] : m_show_func_map) {
        ImGui::Separator();
        func();
    }

    ImGui::PopItemWidth(); // 和PushItemWidt相对应

    ImGui::End();
}

void EditorUI::ResetState() { m_b_need_reload = false; }

void EditorUI::RegisterUIFunc(std::string_view _name, std::function<void()>&& _func) {
    m_show_func_map[_name] = std::move(_func);
}

void EditorUI::UnregisterUIFunc(std::string_view _name) { m_show_func_map.erase(_name); }

} // namespace Moer