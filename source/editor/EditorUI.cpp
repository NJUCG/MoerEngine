#include "EditorUI.h"

// Runtime
#include "config/ConfigManager.h"
#include "log/LogSystem.h"
#include "window/WindowInput.h"

// Editor
#include "EditorUIStyle.h"
#include "scene/Scene.h"
#include "scene/SceneGlobalEntry.h"
#include "scene_editing_ui/SceneFileDialog.h"

// 3rd party (std)
#include <imgui.h>
#include <imgui_internal.h>
#include <string_view>

#if WITH_PROFILE
#include "Profile.h"
#endif

using namespace Moer::Render;

namespace Moer {

EditorUI::EditorUI(UniquePtr<Render::UIRenderer> renderer, SharedPtr<EditorConfig> editor_config) :
    m_ui_renderer(std::move(renderer)),
    m_config(editor_config),
    m_raster_ui(editor_config->raster_config),
    m_raytracing_ui(editor_config->raytracing_config),
    m_scene_editing_ui(editor_config->scene_test_case_config, m_b_need_reload) {

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
void EditorUI::DrawPassAndChildren(const char* parent_name, int depth) {
    for (int i = 0; i < g_pass_history_count; i++) {
        auto& h = g_pass_history[i];
        if (!h.active)
            continue;
        if (strcmp(h.parent_name, parent_name) != 0)
            continue;
        if (h.avg_ms < 0.001f && h.max_ms < 0.001f)
            continue;

        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        ImGui::Indent((1 + depth) * 20.0f);
        ImGui::Text("%s", h.name);
        ImGui::Unindent((1 + depth) * 20.0f);

        ImGui::TableSetColumnIndex(1);
        if (h.avg_ms > 5.0f)
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%.4f", h.avg_ms);
        else if (h.avg_ms > 2.0f)
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "%.4f", h.avg_ms);
        else
            ImGui::Text("%.4f", h.avg_ms);

        ImGui::TableSetColumnIndex(2);
        ImGui::Text("%.4f", h.max_ms);

        ImGui::TableSetColumnIndex(3);
        float ordered[60];
        for (int j = 0; j < 60; j++) {
            ordered[j] = h.samples[(h.write_idx + j) % 60];
        }
        char plot_label[64];
        snprintf(plot_label, sizeof(plot_label), "##pass_%d", i);
        float plot_max = h.max_ms > 0.0f ? h.max_ms * 1.2f : 1.0f;
        ImGui::PlotLines(plot_label, ordered, 60, 0, nullptr, 0.0f, plot_max, ImVec2(-1, 28));

        DrawPassAndChildren(h.name, depth + 1);
    }
}

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
        static float view_max_y = 5.0f;

        std::vector<TimePoint> snapshot;
        {
            std::lock_guard<std::mutex> lock(g_history_mtx);
            if (g_history_data.empty())
                return;
            snapshot.assign(g_history_data.begin(), g_history_data.end());
        }

        float plot_width = ImGui::GetContentRegionAvail().x;
        for (int s = 0; s < SOURCE_COUNT; ++s) {
            struct PlotContext {
                int                     s_idx;
                std::vector<TimePoint>* d;
            };
            PlotContext ctx = {s, &snapshot};

            auto getter = [](void* data, int idx) -> float {
                auto* p = (PlotContext*)data;
                return p->d->operator[](idx).values[p->s_idx];
            };

            float last_val = snapshot.back().values[s];
            if (last_val * 1.2f > view_max_y)
                view_max_y = last_val * 1.2f;

            ImGui::PushStyleColor(ImGuiCol_PlotLines, g_UIConfigs[s].color);
            ImGui::PlotLines(
                "##", getter, &ctx, (int)snapshot.size(), 0, nullptr, 0.0f, view_max_y, ImVec2(plot_width, 80)
            );
            ImGui::PopStyleColor();

            ImGui::SameLine();
            ImGui::Text("%s: %.2f MB", g_UIConfigs[s].label, last_val);
        }
    }

    ImGui::Spacing();

    if (ImGui::CollapsingHeader("GPU Pass Timing", ImGuiTreeNodeFlags_DefaultOpen)) {
        std::lock_guard<std::mutex> lock(g_pass_history_mtx);

        if (g_pass_history_count == 0) {
            ImGui::TextDisabled("No GPU data yet...");
        } else {
            ImGui::BeginTable(
                "PassTable",
                4,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                ImVec2(0, 300)
            );
            ImGui::TableSetupColumn("Pass", ImGuiTableColumnFlags_WidthFixed, 260);
            ImGui::TableSetupColumn("Avg ms", ImGuiTableColumnFlags_WidthFixed, 70);
            ImGui::TableSetupColumn("Max ms", ImGuiTableColumnFlags_WidthFixed, 70);
            ImGui::TableSetupColumn("Last 60 frames", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            DrawPassAndChildren("", 0);

            ImGui::EndTable();
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("Memory Hotspots Export:");

    if (ImGui::Button("Quick Dump (Hex Only)")) {
        WriteHotspots(false);
    }

    ImGui::SameLine();

    if (ImGui::Button("Full Dump (With Symbols)")) {
        WriteHotspots(true);
    }

    ImGui::Spacing();
    ImGui::End();
}
#endif

void EditorUI::TickUI() {

    // 注：Resolution表示整个窗口的大小（不包含windows标题栏）；SceneColor只表示场景渲染区域的大小
    // 更新SceneColor的分辨率
    m_config->aspect_ratio = (m_scene_color_resolution.x + EPS) / (m_scene_color_resolution.y + EPS);

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
            ImGui::MenuItem("Scene Editing", nullptr, &m_b_show_scene_editing);
            // ImGui::MenuItem("Inspector", nullptr, &m_m_b_show_inspector_window);
            // ImGui::MenuItem("Demo", nullptr, &m_b_show_demo);
#if WITH_PROFILE
            ImGui::MenuItem("Memory Profiler", nullptr, &m_b_show_memory_profiler);
#endif
            ImGui::EndMenu();
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
    ShowSceneEditing();

    m_ui_renderer->EndGUIFrame();
}

void EditorUI::ShowSceneEditing() {
    m_scene_editing_ui.ShowWindow(&m_b_show_scene_editing);
}

void EditorUI::RenderGUI(Render::CommandList& cmd_list, const Render::TextureView& final_output) {
    m_ui_renderer->RenderGUI(cmd_list, final_output);
}

void EditorUI::PresentWindows() {
    m_ui_renderer->PresentWindows();
}

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
    if (!m_b_show_scene_color) {
        m_b_scene_color_mouse_captured = false;
        WindowInput::Get().is_active   = false;
        return;
    }
    if (!ImGui::Begin("Scene Color", &m_b_show_scene_color, window_flags)) {
        // Should not call ImGui::End() here
        m_b_scene_color_mouse_captured = false;
        WindowInput::Get().is_active   = false;
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
    // 鼠标按下起点在SceneColor内时，才捕获本次拖拽并控制摄像机
    static constexpr float border = 4.f;

    const float  scene_color_top = menu_rect.Max.y + (m_b_separate_window ? 0.f : menu_bar);
    const ImVec2 scene_color_min = ImVec2(window_rect.Min.x + border, scene_color_top + border);
    const ImVec2 scene_color_max = ImVec2(
        window_rect.Min.x + m_scene_color_resolution.x - border,
        scene_color_top + m_scene_color_resolution.y - border
    );
    const ImVec2 mouse_pos = ImGui::GetMousePos();

    const bool b_scene_color_hovered = mouse_pos.x > scene_color_min.x && mouse_pos.x < scene_color_max.x &&
                                       mouse_pos.y > scene_color_min.y && mouse_pos.y < scene_color_max.y;

    const bool b_mouse_clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
                                 ImGui::IsMouseClicked(ImGuiMouseButton_Middle) ||
                                 ImGui::IsMouseClicked(ImGuiMouseButton_Right);
    const bool b_mouse_down    = WindowInput::Get().mouse_button_state[MouseButtons::Left] ||
                                 WindowInput::Get().mouse_button_state[MouseButtons::Middle] ||
                                 WindowInput::Get().mouse_button_state[MouseButtons::Right];

    if (!b_mouse_down) {
        m_b_scene_color_mouse_captured = false;
    } else if (!m_b_scene_color_mouse_captured && b_mouse_clicked && b_scene_color_hovered) {
        m_b_scene_color_mouse_captured     = true;
        WindowInput::Get().is_cursor_dirty = true;
        WindowInput::Get().cursor_delta_x  = 0.f;
        WindowInput::Get().cursor_delta_y  = 0.f;
    }

    WindowInput::Get().is_active =
        m_b_scene_color_mouse_captured ||
        (b_scene_color_hovered && WindowInput::Get().key_button_switch_state[KeyButtons::F]);

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
            std::string selected_path;
            switch (OpenSceneFileDialog(selected_path)) {
                case ESceneFileDialogResult::Selected:
                LOG_INFO("User selected file: {}", selected_path);

                // Prepare for reload
                m_b_need_reload      = true;
                m_config->scene_path = std::move(selected_path);
                break;
                case ESceneFileDialogResult::Cancelled:
                    LOG_INFO("User pressed cancel.");
                    break;
                case ESceneFileDialogResult::Error:
                    break;
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

} // namespace Moer