#include "EditorUI.h"

// Runtime
#include "config/ConfigManager.h"
#include "log/LogSystem.h"
#include "window/WindowInput.h"

// Editor
#include "EditorUIStyle.h"
#include "scene/Scene.h"
#include "scene/SceneGlobalEntry.h"

#include "trace/Trace.h"

// 3rd party (std)
#include <imgui.h>
#include <imgui_internal.h>
#include <nfd.hpp>
#include <string_view>

#if WITH_PROFILE
#include "profile.h"
#endif

using namespace Moer::Render;

namespace Moer {

EditorUI::EditorUI(UniquePtr<Render::UIRenderer> renderer, SharedPtr<EditorConfig> editor_config) :
    m_ui_renderer(std::move(renderer)),
    m_config(editor_config),
    m_raster_ui(editor_config->raster_config),
    m_raytracing_ui(editor_config->raytracing_config) {

    // Load Config
    InitFromConfigManager();

    // Init Style
    EditorUIStyle::ApplyDefaultStyle();
}

void EditorUI::InitFromConfigManager() {
    auto config = ConfigManager::GetInstance().GetConfig();

    // render method
    if (config.engine.render.default_render_method == "Raster") {
        m_config->selected_render_method = ERenderMethod::Raster;
    } else if (config.engine.render.default_render_method == "Raytracing") {
        m_config->selected_render_method = ERenderMethod::Raytracing;
    } else {
        LOG_WARNING(
            "Invalid default render method: {}. Use Raster instead.",
            config.engine.render.default_render_method
        );
        m_config->selected_render_method = ERenderMethod::Raster;
    }

    // scene path
    m_config->scene_path = config.engine.scene.scene_path;
}

#if WITH_PROFILE
void EditorUI::ShowMemoryProfiler(bool* p_open) {
    Profile_TickSample();

    if (!ImGui::Begin("Memory Profiler", p_open)) {
        ImGui::End();
        return;
    }

    if (ImGui::CollapsingHeader("Real-time Metrics", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::BeginTable("MetricsTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Source");
            ImGui::TableSetupColumn("Current (MB)");
            ImGui::TableSetupColumn("Peak (MB)");
            ImGui::TableSetupColumn("Color", ImGuiTableColumnFlags_WidthFixed, 40.0f);
            ImGui::TableHeadersRow();

            for (int i = 0; i < SOURCE_COUNT; ++i) {
                const auto& config    = g_UIConfigs[i];
                float       currentMB = Profile_GetBytesBySource(config.source) / 1048576.0f;
                float       peakMB    = Profile_GetPeakBytesBySource(config.source) / 1048576.0f;

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%s", config.label);

                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%.3f", currentMB);

                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%.3f", peakMB);

                ImGui::TableSetColumnIndex(3);
                ImGui::ColorButton(
                    config.label, config.color, ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoInputs
                );
            }
            ImGui::EndTable();
        }
    }

    ImGui::Spacing();

    if (ImGui::CollapsingHeader("Live Memory Graph")) {
        static float max_y = 5.0f;

        std::lock_guard<std::mutex> lock(g_history_mtx);
        if (!g_history_data.empty()) {
            float plot_width = ImGui::GetContentRegionAvail().x;

            for (int s = 0; s < SOURCE_COUNT; ++s) {
                struct PlotContext {
                    int                    source_idx;
                    std::deque<TimePoint>* data;
                };
                PlotContext ctx = {s, &g_history_data};

                auto deque_getter = [](void* data, int idx) -> float {
                    PlotContext* p = static_cast<PlotContext*>(data);
                    return p->data->at(idx).values[p->source_idx];
                };

                float current_max = 0.0f;
                for (const auto& tp : g_history_data) {
                    if (tp.values[s] > current_max)
                        current_max = tp.values[s];
                }
                if (current_max * 1.2f > max_y)
                    max_y = current_max * 1.2f;

                char label[128];
                sprintf_s(label, "%s: %.2f MB", g_UIConfigs[s].label, g_history_data.back().values[s]);

                ImGui::PushStyleColor(ImGuiCol_PlotLines, g_UIConfigs[s].color);
                ImGui::PlotLines(
                    label,
                    deque_getter,
                    &ctx,
                    (int)g_history_data.size(),
                    0,
                    nullptr,
                    0.0f,
                    max_y,
                    ImVec2(plot_width, 80)
                );
                ImGui::PopStyleColor();
            }
        }
    }

    ImGui::Spacing();

    if (ImGui::CollapsingHeader("Memory Hotspots (Top 20)")) {
        static MemorySource current_filter = MemorySource::Editor;

        if (ImGui::BeginTabBar("HotspotSourceTabs")) {
            for (int i = 0; i < SOURCE_COUNT; ++i) {
                if (ImGui::BeginTabItem(g_UIConfigs[i].label)) {
                    current_filter = g_UIConfigs[i].source;
                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
        }

        auto hotspots = GetHotspots(20, current_filter);

        if (ImGui::BeginTable(
                "HotspotTable",
                3,
                ImGuiTableFlags_Resizable | ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                    ImGuiTableFlags_ScrollY,
                ImVec2(0, 300)
            )) {
            ImGui::TableSetupColumn("Size (MB)", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGui::TableSetupColumn("CallStack");
            ImGui::TableHeadersRow();

            for (const auto& snap : hotspots) {
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%.3f", snap.total_size / 1048576.0f);

                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%zu", snap.alloc_count);

                ImGui::TableSetColumnIndex(2);

                size_t      first_line = snap.stack_str.find('\n');
                std::string preview    = snap.stack_str.substr(0, first_line);

                if (ImGui::Selectable(preview.c_str(), false)) {
                    ImGui::SetClipboardText(snap.stack_str.c_str());
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Click to copy FULL stack trace\n\n%s", snap.stack_str.c_str());
                }
            }
            ImGui::EndTable();
        }
    }

    ImGui::End();
}
#endif

void EditorUI::TickUI() {

    // 注：Resolution表示整个窗口的大小（不包含windows标题栏）；SceneColor只表示场景渲染区域的大小
    // 更新SceneColor的分辨率
    m_config->aspect_ratio = (m_scene_color_resolution.x + EPS) / (m_scene_color_resolution.y + EPS);

    m_ui_renderer->BeginGUIFrame();
    ImGuiIO& io = ImGui::GetIO();

    if (ImGui::IsKeyPressed(ImGuiKey_F5, false) && !m_config->play_mode_enabled) {
        m_config->play_mode_enabled       = true;
        m_config->play_mode_capture_input = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F8, false) && m_config->play_mode_enabled) {
        m_config->play_mode_capture_input = !m_config->play_mode_capture_input;
    }

    const bool play_capture = m_config->play_mode_enabled && m_config->play_mode_capture_input;
    WindowInput::Get().force_cursor_hidden       = play_capture;
    WindowInput::Get().play_mode_camera_control  = play_capture;
    if (!play_capture) {
        WindowInput::Get().force_cursor_hidden      = false;
        WindowInput::Get().play_mode_camera_control = false;
    }

    if (play_capture) {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        m_scene_color_pos             = {0.0f, 0.0f};
        m_scene_color_resolution      = {viewport->WorkSize.x, viewport->WorkSize.y};
        m_config->aspect_ratio        = (m_scene_color_resolution.x + EPS) / (m_scene_color_resolution.y + EPS);
        WindowInput::Get().is_active  = true;

        ShowOverlay();
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
#if WITH_PROFILE
            ImGui::MenuItem("Memory Profiler", nullptr, &m_b_show_memory_profiler);
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
                WindowInput::Get().force_cursor_hidden      = false;
                WindowInput::Get().play_mode_camera_control = false;
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
#if WITH_PROFILE
    if (m_b_show_memory_profiler) {
        ShowMemoryProfiler(&m_b_show_memory_profiler);
    }
#endif
    ResetState();
    ShowSceneColor();
    ShowConfig();
    ShowOverlay();

    m_ui_renderer->EndGUIFrame();
}

void EditorUI::RenderGUI(Render::CommandList& cmd_list, const Render::TextureView& final_output) {
    m_ui_renderer->RenderGUI(cmd_list, final_output);
}

void EditorUI::PresentWindows() {
    m_ui_renderer->PresentWindows();
}

bool EditorUI::IsSeperateWindow() const {
    auto* current_window = ImGui::FindWindowByName("Scene Color");
    if (!current_window) {
        return false;
    }
    return current_window->ParentWindow == nullptr;
}

TextureView EditorUI::GetWindowFrameBuffer() {
    auto* current_window = ImGui::FindWindowByName("Scene Color");
    if (!current_window) {
        return TextureView();
    }
    if (current_window->ParentWindow == nullptr) {
        return m_ui_renderer->GetWindowFrameBuffer(current_window->Viewport);
    }
    return TextureView();
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
        LOG_WARNING("Please bind a scene by `SceneGlobalEntry::Get().BindScene(scene)`");
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
            SetShowSubUI(false);
        }
    }

    { // Scene Path
        size_t      last_slash = m_config->scene_path.find_last_of("/\\");
        std::string scene_name = (last_slash == std::string::npos) ?
                                     m_config->scene_path :
                                     m_config->scene_path.substr(last_slash + 1);
        if (ImGui::Button("Open Scene")) {
            NFD::UniquePath        selected_path = nullptr;
            Array<nfdfilteritem_t> filters       = {
                {"All Support Formats", "glb,gltf,fbx,obj,dae"},
                {"glTF 2.0", "glb,gltf"},
                {"FBX", "fbx"},
                {"Wavefront", "obj"},
                {"Moer Renderer Scene (WIP)", "json"},
            };
            nfdresult_t result = NFD::OpenDialog(selected_path, filters.data(), filters.size());
            if (result == NFD_OKAY) {
                LOG_INFO("User selected file: {}", selected_path.get());

                // Prepare for reload
                m_b_need_reload      = true;
                m_config->scene_path = selected_path.get();
            } else if (result == NFD_CANCEL) {
                LOG_INFO("User pressed cancel.");
            } else {
                LOG_ERROR("NFD Error: {}", NFD_GetError());
            }
        }
        ImGui::SameLine();
        ImGui::Text("Current: [%s]", scene_name.c_str());
    }

    if (ImGui::TreeNode("Camera")) {

        ImGui::SliderFloat("Speed (log10)", &m_config->camera_speed_log10, -1.f, 2.6f);
        ImGui::SliderFloat("Fov Y", &m_config->camera_fovy, 1.f, 160.f);

        ImGui::SliderFloat("Near Clip (log10)", &m_config->camera_near_clip_log10, -4.f, 1.f);
        ImGui::SliderFloat("Far Clip (log10)", &m_config->camera_far_clip_log10, 1.f, 6.f);
        m_config->camera_near_clip_log10 =
            std::min(m_config->camera_near_clip_log10, m_config->camera_far_clip_log10 - 0.1f);

        ImGui::TreePop();
    }

    if (m_b_show_sub_ui) {
        ImGui::Separator();
        switch (m_config->selected_render_method) {
            case ERenderMethod::Raster:
                m_raster_ui.ShowConfig();
                break;
            case ERenderMethod::Raytracing:
                m_raytracing_ui.ShowConfig();
                break;
            default:
                break;
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

    // Custom Func
    for (auto& [name, func] : m_show_func_map) {
        ImGui::Separator();
        func();
    }

    ImGui::PopItemWidth(); // 和PushItemWidt相对应

    ImGui::End();
}

void EditorUI::ResetState() {
    m_b_need_reload = false;
}

void EditorUI::RegisterUIFunc(std::string _name, std::function<void()>&& _func) {
    m_show_func_map[_name] = std::move(_func);
}

void EditorUI::UnregisterUIFunc(std::string _name) {
    m_show_func_map.erase(_name);
}

void EditorUI::RegisterOverlayFunc(std::string _name, std::function<void()>&& _func) {
    m_overlay_func_map[_name] = std::move(_func);
}

void EditorUI::UnregisterOverlayFunc(std::string _name) {
    m_overlay_func_map.erase(_name);
}

void EditorUI::ShowOverlay() {
    for (auto& [name, func] : m_overlay_func_map) {
        func();
    }
}

} // namespace Moer
