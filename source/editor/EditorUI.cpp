#include "EditorUI.h"

// Runtime
#include "file/FileDialog.h"
#include "log/LogSystem.h"
#include "window/WindowInput.h"

// Editor
#include "EditorUIStyle.h"
#include "scene/Scene.h"
#include "scene/SceneGlobalEntry.h"

// 3rd party (std)
#include <algorithm>
#include <imgui.h>
#include <imgui_internal.h>
#include <string_view>

using namespace Moer::Render;

namespace Moer {

EditorUI::EditorUI(UniquePtr<Render::UIRenderer> renderer, SharedPtr<EditorConfig> editor_config) :
    m_ui_renderer(std::move(renderer)),
    m_config(editor_config) {
    // Init Style
    EditorUIStyle::ApplyDefaultStyle();
}

void EditorUI::TickUI() {

    // 注：Resolution表示整个窗口的大小（不包含windows标题栏）；SceneColor只表示场景渲染区域的大小
    // 更新SceneColor的分辨率
    m_config->aspect_ratio = (m_scene_color_resolution.x + EPS) / (m_scene_color_resolution.y + EPS);

    m_ui_renderer->BeginGUIFrame();
    ImGuiIO& io = ImGui::GetIO();

    const Render::ImGuiIOInputSnapshot& input_snapshot = m_ui_renderer->GetInputSnapshot();
    m_scene_color_hovered = false;

    if (input_snapshot.f5_pressed && !m_config->play_mode_enabled) {
        m_config->play_mode_enabled       = true;
        m_config->play_mode_capture_input = true;
    }
    if (input_snapshot.f8_pressed && m_config->play_mode_enabled) {
        m_config->play_mode_capture_input = !m_config->play_mode_capture_input;
    }

    const bool play_capture = m_config->play_mode_enabled && m_config->play_mode_capture_input;

    if (play_capture) {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        m_scene_color_pos             = {0.0f, 0.0f};
        m_scene_color_resolution      = {viewport->WorkSize.x, viewport->WorkSize.y};
        m_config->aspect_ratio        = (m_scene_color_resolution.x + EPS) / (m_scene_color_resolution.y + EPS);
        m_scene_color_hovered         = true;

        ShowOverlay();
        ApplyInputSnapshot();
        m_ui_renderer->EndGUIFrame();
        return;
    }

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
    if (!opt_padding)
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("Editor Menu", &m_b_show, window_flags);
    if (!opt_padding)
        ImGui::PopStyleVar();

    if (opt_fullscreen)
        ImGui::PopStyleVar(2);

    // Submit the DockSpace
    if (io.ConfigFlags & ImGuiConfigFlags_DockingEnable) {
        ImGuiID dockspace_id = ImGui::GetID("Docking Main");
        ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), dockspace_flags);
        SetupDefaultDockLayout(dockspace_id);
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
            if (m_get_console_window_visible && m_set_console_window_visible) {
                bool show_console = m_get_console_window_visible();
                if (ImGui::MenuItem("Console", nullptr, &show_console)) {
                    m_set_console_window_visible(show_console);
                }
            }
            // ImGui::MenuItem("Inspector", nullptr, &m_m_b_show_inspector_window);
            // ImGui::MenuItem("Demo", nullptr, &m_b_show_demo);
#if WITH_PROFILE
            bool show_runtime_profiler = m_runtime_profiler.IsOpen();
            if (ImGui::MenuItem("runtime_profiler", nullptr, &show_runtime_profiler)) {
                m_runtime_profiler.SetOpen(show_runtime_profiler);
            }
#endif
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (!m_config->play_mode_enabled) {
            if (ImGui::Button("Play (F5)")) {
                m_config->play_mode_enabled       = true;
                m_config->play_mode_capture_input = true;
            }
        } else {
            if (ImGui::Button("Stop Play")) {
                m_config->play_mode_enabled       = false;
                m_config->play_mode_capture_input = false;
            }
            ImGui::SameLine();
            const char* toggle_label =
                m_config->play_mode_capture_input ? "Eject (F8)" : "Possess (F8)";
            if (ImGui::Button(toggle_label)) {
                m_config->play_mode_capture_input = !m_config->play_mode_capture_input;
            }
        }
        ImGui::EndMenuBar();
    }
    ImGui::End();
    ResetState();
    ShowSceneColor();
    ShowConfig();
#if WITH_PROFILE
    m_runtime_profiler.TickUI();
#endif
    ShowOverlay();

    ApplyInputSnapshot();
    m_ui_renderer->EndGUIFrame();
}

void EditorUI::SetupDefaultDockLayout(ImGuiID dockspace_id) {
    ImGuiDockNode* dockspace_node = ImGui::DockBuilderGetNode(dockspace_id);
    if (!dockspace_node || dockspace_node->IsSplitNode() || !dockspace_node->Windows.empty()) {
        return;
    }

    ImGuiID main_id = dockspace_id;
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, ImGui::GetMainViewport()->WorkSize);

    ImGuiID right_id = ImGui::DockBuilderSplitNode(main_id, ImGuiDir_Right, 0.28f, nullptr, &main_id);
    ImGuiID config_id = right_id;
    ImGuiID profiler_id = ImGui::DockBuilderSplitNode(right_id, ImGuiDir_Down, 0.50f, nullptr, &config_id);

    ImGui::DockBuilderDockWindow("Scene Color", main_id);
    ImGui::DockBuilderDockWindow("Configs", config_id);
#if WITH_PROFILE
    ImGui::DockBuilderDockWindow("runtime_profiler", profiler_id);
#endif
    ImGui::DockBuilderDockWindow("Console", profiler_id);
    ImGui::DockBuilderFinish(dockspace_id);
}

void EditorUI::RenderGUI(Render::CommandList& cmd_list, const Render::TextureView& final_output) {
    m_ui_renderer->RenderGUI(cmd_list, final_output);
}

void EditorUI::PresentWindows() {
    m_ui_renderer->PresentWindows();
}

EditorUI::SceneWindowTarget EditorUI::GetSceneWindowTarget() {
    const Render::UIRenderer::WindowRenderTarget target =
        m_ui_renderer->GetWindowRenderTarget("Scene Color");
    if (!target.is_separate_window || !target.frame_buffer.GetTexture()) {
        return {};
    }

    return SceneWindowTarget{
        .is_separate_window = target.is_separate_window,
        .frame_buffer       = target.frame_buffer,
    };
}

void EditorUI::ShowSceneColor() {
    ImGuiIO&         io           = ImGui::GetIO();
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_MenuBar;

    const ImGuiViewport* main_viewport = ImGui::GetMainViewport();
    if (!m_b_show_scene_color) {
        return;
    }
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
        ImGui::GetMousePos().x - parent_rect.Min.x, ImGui::GetMousePos().y - parent_rect.Min.y
    );
    static uint border = 4;

    m_scene_color_hovered = mouse_pos.x > m_scene_color_pos.x + border &&
                            mouse_pos.x < m_scene_color_pos.x + m_scene_color_resolution.x - border &&
                            mouse_pos.y > m_scene_color_pos.y + border &&
                            mouse_pos.y < m_scene_color_pos.y + m_scene_color_resolution.y - border;

    ImGui::End();
}

void EditorUI::ShowConfig() {
    ImGuiIO&         io           = ImGui::GetIO();
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_None;

    if (!m_b_show_config) {
        return;
    }
    if (!ImGui::Begin("Configs", &m_b_show_config, window_flags)) {
        return;
    }

    ImGui::PushItemWidth(120); // 设置所有组件width为120

    EditorUIStyle::ShowStyleSelector("Style##Default");
    if (ImGui::TreeNode("Trace")) {
        const Moer::Trace::Stats trace_stats = Moer::Trace::GetStats();
        ImGui::Text("Enabled: %s", trace_stats.enabled ? "Yes" : "No");
        ImGui::Text("Recording: %s", trace_stats.recording ? "On" : "Off");
        if (trace_stats.recording) {
            if (ImGui::Button("Stop Trace")) {
                Moer::Trace::StopRecording();
            }
        } else {
            if (ImGui::Button("Start Trace")) {
                Moer::Trace::StartRecording();
            }
        }
        ImGui::Text("Connected: %s", trace_stats.connected ? "Yes" : "No");
        ImGui::Text("Queued Events: %llu", static_cast<unsigned long long>(trace_stats.queued_events));
        ImGui::Text("Dropped Events: %llu", static_cast<unsigned long long>(trace_stats.dropped_events));
        ImGui::TreePop();
    }

    // MARK: Common Configs

    /////////////////////////////////////////////////// Begin Disabled Here
    // 避免场景加载一半，切换场景或渲染器，导致崩溃
    Scene* scene = SceneGlobalEntry::Get().GetScene();

    bool is_scene_found_but_not_ready = false;
    if (scene) {
        is_scene_found_but_not_ready = !scene->IsReady() && scene->IsStartLoading();
    } else {
        LOG_WARNING(MOER_TEXT("Please bind a scene by `SceneGlobalEntry::Get().BindScene(scene)`"));
    }
    ImGui::BeginDisabled(is_scene_found_but_not_ready);

    // Render Method
    {
        auto last_selected_render_method = m_config->selected_render_method;
        if (ImGui::BeginCombo(
                "Render Method",
                k_render_method_names[static_cast<uint>(m_config->selected_render_method)].data()
            )) {
            for (int i = 0; i < IM_ARRAYSIZE(k_render_method_names); i++) {
                const bool is_selected = (m_config->selected_render_method == static_cast<ERenderMethod>(i));
                if (ImGui::Selectable(k_render_method_names[i].data(), is_selected)) {
                    m_config->selected_render_method = static_cast<ERenderMethod>(i);
                }
                if (is_selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        if (last_selected_render_method != m_config->selected_render_method) {
            m_b_need_reload = true;
        }
    }

    { // Scene Path
        size_t      last_slash = m_config->scene_path.find_last_of("/\\");
        std::string scene_name = (last_slash == std::string::npos) ?
                                     m_config->scene_path :
                                     m_config->scene_path.substr(last_slash + 1);
        if (ImGui::Button("Open Scene")) {
            static constexpr std::array<FileDialog::Filter, 5> scene_filters = {{{
                "All Support Formats",
                "glb,gltf,fbx,obj,dae"
            }, {
                "glTF 2.0",
                "glb,gltf"
            }, {
                "FBX",
                "fbx"
            }, {
                "Wavefront",
                "obj"
            }, {
                "Moer Renderer Scene (WIP)",
                "json"
            }}};
            const FileDialog::OpenFileResult result = FileDialog::OpenFile({.filters = scene_filters});
            if (result.status == FileDialog::EOpenFileStatus::Success) {
                LOG_INFO(MOER_TEXT("User selected file: {}"), result.path.string());

                // Prepare for reload
                m_b_need_reload      = true;
                m_config->scene_path = result.path.string();
            } else if (result.status == FileDialog::EOpenFileStatus::Cancelled) {
                LOG_INFO(MOER_TEXT("User pressed cancel."));
            }
        }
        ImGui::SameLine();
        ImGui::Text("Current: [%s]", scene_name.c_str());
    }

    if (ImGui::TreeNode("Camera")) {

        ImGui::SliderFloat("Speed (log10)", &m_config->camera_speed_log10, -1.f, 2.6f);
        ImGui::SliderFloat("Fov Y", &m_config->camera_fovy, 1.f, 160.f);

        ImGui::SliderFloat("Near Clip (log10)", &m_config->camera_near_clip_log10, -4.f, 0.99f);
        ImGui::SliderFloat("Far Clip (log10)", &m_config->camera_far_clip_log10, 0.f, 4.f);
        m_config->camera_near_clip_log10 =
            std::min(m_config->camera_near_clip_log10, m_config->camera_far_clip_log10 - 0.1f);

        ImGui::TreePop();
    }

    const std::string active_renderer = std::string(
        k_render_method_names[static_cast<uint>(m_config->selected_render_method)]
    );
    if (const auto renderer_iter = m_renderer_config_sections.find(active_renderer);
        renderer_iter != m_renderer_config_sections.end() && !renderer_iter->second.empty()) {
        Array<std::string> section_names;
        section_names.reserve(renderer_iter->second.size());
        for (const auto& [section_name, func] : renderer_iter->second) {
            section_names.push_back(section_name);
        }
        std::sort(section_names.begin(), section_names.end());

        ImGui::SeparatorText(active_renderer.c_str());
        for (const std::string& section_name : section_names) {
            auto section_iter = renderer_iter->second.find(section_name);
            if (section_iter == renderer_iter->second.end()) {
                continue;
            }
            if (ImGui::TreeNode(section_name.c_str())) {
                section_iter->second();
                ImGui::TreePop();
            }
        }
    }

    ImGui::Separator();

    ImGui::EndDisabled();
    if (is_scene_found_but_not_ready) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Scene is loading... Please wait.");
    }
    /////////////////////////////////////////////////// End Disabled Here

    ImGui::Text("Infos: ");
    ImGui::Text("\tFPS: %.1f", io.Framerate);
    ImGui::Text("\tFrame Time: %.1f ms", 1000.0f / io.Framerate);

    ImGui::PopItemWidth(); // 和PushItemWidt相对应

    ImGui::End();
}

void EditorUI::ResetState() {
    m_b_need_reload = false;
}

void EditorUI::RegisterRendererConfigSection(
    std::string renderer_name,
    std::string section_name,
    std::function<void()>&& func
) {
    m_renderer_config_sections[std::move(renderer_name)][std::move(section_name)] = std::move(func);
}

void EditorUI::UnregisterRendererConfigSection(std::string renderer_name, std::string section_name) {
    auto renderer_iter = m_renderer_config_sections.find(renderer_name);
    if (renderer_iter == m_renderer_config_sections.end()) {
        return;
    }

    renderer_iter->second.erase(section_name);
    if (renderer_iter->second.empty()) {
        m_renderer_config_sections.erase(renderer_iter);
    }
}

void EditorUI::RegisterOverlayFunc(std::string _name, std::function<void()>&& _func) {
    m_overlay_func_map[_name] = std::move(_func);
}

void EditorUI::UnregisterOverlayFunc(std::string _name) {
    m_overlay_func_map.erase(_name);
}

void EditorUI::BindConsoleWindowState(std::function<bool()> getter, std::function<void(bool)> setter) {
    m_get_console_window_visible = std::move(getter);
    m_set_console_window_visible = std::move(setter);
}

void EditorUI::ShowOverlay() {
    for (auto& [name, func] : m_overlay_func_map) {
        func();
    }
}

void EditorUI::ApplyInputSnapshot() {
    Render::ApplyImGuiIOInputToWindowInput(
        m_ui_renderer->GetInputSnapshot(),
        Render::ImGuiIOInputApplyParams{
            .scene_active            = m_scene_color_hovered,
            .play_capture            = m_config->play_mode_enabled && m_config->play_mode_capture_input,
            .external_key_block      = false,
            .external_cursor_visible = false,
        }
    );
}

} // namespace Moer
