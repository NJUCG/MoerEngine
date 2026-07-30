#include "EditorUI.h"

// 构建编辑器 ImGui Dockspace，协调顶层工具，并跟踪当前活动的渲染视口。

// Runtime
#include "log/LogSystem.h"
#include "window/WindowInput.h"

// Editor
#include "EditorUISettings.h"
#include "EditorUIStyle.h"
#include "scene/Scene.h"
#include "scene_editing_ui/SceneFileDialog.h"
#include "window/WindowContext.h"

// 3rd party (std)
#include <algorithm>
#include <imgui.h>
#include <imgui_internal.h>
#include <string_view>

#if WITH_PROFILE
#include "Profile.h"
#endif

using namespace Moer::Render;

namespace Moer {

namespace {

std::string GetSceneFileName(std::string_view scene_path) {
    if (scene_path.empty()) {
        return "No Scene";
    }

    const size_t last_slash = scene_path.find_last_of("\\/");
    return last_slash == std::string::npos ? std::string(scene_path) :
                                             std::string(scene_path.substr(last_slash + 1));
}

std::string TruncateMenuValue(std::string_view value) {
    constexpr size_t k_max_display_length      = 15;
    constexpr size_t k_truncated_prefix_length = 10;

    if (value.size() <= k_max_display_length) {
        return std::string(value);
    }

    return std::string(value.substr(0, k_truncated_prefix_length)) + "...";
}

std::string BuildFileMenuLabel(std::string_view scene_path) {
    return "File: [" + TruncateMenuValue(GetSceneFileName(scene_path)) + "]";
}

std::string BuildRenderMethodMenuLabel(ERenderMethod render_method) {
    return "Render Method: [" + std::string(k_render_method_names[static_cast<uint>(render_method)]) + "]";
}

} // namespace

EditorUI::EditorUI(
    UniquePtr<Render::UIRenderer>         renderer,
    SharedPtr<EditorConfig>               editor_config,
    const remote::RemoteModuleController& remote_controller,
    Engine&                               engine,
    ProfileDump::ProfileDocumentLoader&   profile_document_loader
) :
    m_raster_ui(editor_config->raster_config),
    m_raytracing_ui(editor_config->raytracing_config),
    m_scene_editing_ui(editor_config->scene_test_case_config),
    m_profile_capture_ui(engine),
    m_profile_viewer_ui(profile_document_loader),
    m_config(editor_config),
    m_remote_controller(remote_controller),
    m_ui_renderer(std::move(renderer)) {

    const EditorWindowVisibilitySettings& window_visibility_settings =
        EditorUISettings::LoadWindowVisibilitySettings();

    const auto has_saved_window_settings = [](const char* window_name) {
        return ImGui::FindWindowSettingsByID(ImHashStr(window_name)) != nullptr;
    };

    if (window_visibility_settings.loaded) {
        m_b_show_scene_color   = window_visibility_settings.scene_color;
        m_b_show_scene_view    = window_visibility_settings.scene_view;
        m_b_show_hierarchy     = window_visibility_settings.hierarchy;
        m_b_show_inspector     = window_visibility_settings.inspector;
        m_b_show_config        = window_visibility_settings.config;
        m_b_show_scene_editing = window_visibility_settings.scene_editing;
        m_b_show_profile_capture =
            window_visibility_settings.profile_capture;
        m_b_show_profile_viewer = window_visibility_settings.profile_viewer;
#if WITH_PROFILE
        m_b_show_memory_profiler = window_visibility_settings.memory_profiler;
#endif
    } else {
        m_b_show_scene_color   = has_saved_window_settings("Game") || has_saved_window_settings("Scene Color");
        m_b_show_scene_view    = has_saved_window_settings("Scene");
        m_b_show_hierarchy     = has_saved_window_settings("Hierarchy");
        m_b_show_inspector     = has_saved_window_settings("Inspector");
        m_b_show_config        = has_saved_window_settings("Configs");
        m_b_show_scene_editing = has_saved_window_settings("Scene Editing");
        m_b_show_profile_capture =
            has_saved_window_settings("Profile Capture");
        m_b_show_profile_viewer = has_saved_window_settings("Profile Viewer");
#if WITH_PROFILE
        m_b_show_memory_profiler = has_saved_window_settings("Memory Profiler");
#endif
    }

    // Init Style
    EditorUIStyle::ApplyDefaultStyle();
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

            const auto getter = [](void* data, int idx) -> float {
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

void EditorUI::TickUI(Scene& scene) {

    ResetFrameState();

    m_ui_renderer->BeginGUIFrame();
    const bool input_frame_accepted =
        m_window_input_tracker.BeginFrame(m_ui_renderer->GetInputSnapshot());
    assert(input_frame_accepted && "Each UI frame must publish one fresh input snapshot.");

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
    ImGuiIO&   io               = ImGui::GetIO();
    const bool is_scene_loading = !scene.IsReady() && scene.IsStartLoading();
    if (io.ConfigFlags & ImGuiConfigFlags_DockingEnable) {
        ImGuiID dockspace_id = ImGui::GetID("Docking Main");
        ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), dockspace_flags);
    }
    if (ImGui::BeginMenuBar()) {
        ShowFileMenu(scene, is_scene_loading);

        const auto        last_selected_render_method = m_config->selected_render_method;
        const std::string render_method_menu_label =
            BuildRenderMethodMenuLabel(m_config->selected_render_method);
        if (ImGui::BeginMenu(render_method_menu_label.c_str())) {
            for (int i = 0; i < IM_ARRAYSIZE(k_render_method_names); i++) {
                const bool is_selected = (m_config->selected_render_method == static_cast<ERenderMethod>(i));
                if (ImGui::MenuItem(
                        k_render_method_names[i].data(), nullptr, is_selected, !is_scene_loading
                    )) {
                    m_config->selected_render_method = static_cast<ERenderMethod>(i);
                }
                if (is_selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndMenu();
        }
        if (last_selected_render_method != m_config->selected_render_method) {
            m_b_need_reload = true;
            SetShowRenderConfigSubUI(false);
        }

        if (ImGui::BeginMenu("Window")) {

            ImGui::MenuItem("Game", nullptr, &m_b_show_scene_color);
            ImGui::MenuItem("Scene", nullptr, &m_b_show_scene_view);
            ImGui::MenuItem("Hierarchy", nullptr, &m_b_show_hierarchy);
            ImGui::MenuItem("Inspector", nullptr, &m_b_show_inspector);
            ImGui::MenuItem("Configs", nullptr, &m_b_show_config);
            ImGui::MenuItem("Scene Editing", nullptr, &m_b_show_scene_editing);
            ImGui::MenuItem(
                "Profile Capture",
                nullptr,
                &m_b_show_profile_capture
            );
            ImGui::MenuItem("Profile Viewer", nullptr, &m_b_show_profile_viewer);
            // ImGui::MenuItem("Demo", nullptr, &m_b_show_demo);
#if WITH_PROFILE
            ImGui::MenuItem("Memory Profiler", nullptr, &m_b_show_memory_profiler);
#endif
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
    ImGui::End();
    if (m_b_show_profile_capture) {
        m_profile_capture_ui.ShowWindow(&m_b_show_profile_capture);
    }
    if (m_b_show_profile_viewer) {
        m_profile_viewer_ui.ShowWindow(&m_b_show_profile_viewer);
    }
#if WITH_PROFILE
    if (m_b_show_memory_profiler) {
        ShowMemoryProfiler(&m_b_show_memory_profiler);
    }
#endif
    m_b_active_viewport_window_seen = false;
    m_b_active_viewport_hovered     = false;
    m_active_input_viewport_id      = 0;
    m_scene_color_resolution        = {0.f, 0.f};
    m_scene_color_pos               = {0.f, 0.f};
    if (m_config->active_viewport_mode == EEditorViewportMode::Game) {
        ShowGameView();
        ShowSceneView(scene);
    } else {
        ShowSceneView(scene);
        ShowGameView();
    }
    if (!m_b_active_viewport_window_seen) {
        m_b_scene_color_mouse_captured = false;
    }
    ShowHierarchy(scene);
    ShowInspector(scene);
    ShowConfig(scene);
    m_remote_examples_ui.ShowWindow(m_remote_controller);
    ShowSceneEditing(scene);

    SyncWindowVisibilitySettings();

    const auto to_extent_component = [](float value) {
        return value > 0.f ? static_cast<uint>(value) : 0u;
    };
    const WindowInputSourceSnapshot& pending_input_source =
        m_window_input_tracker.GetPendingSource();
    const bool any_mouse_button_down =
        pending_input_source.mouse_button_down[MouseButtons::Left] ||
        pending_input_source.mouse_button_down[MouseButtons::Middle] ||
        pending_input_source.mouse_button_down[MouseButtons::Right];
    m_free_look_capture_viewport_id = ResolveFreeLookCaptureViewportId(
        pending_input_source.focused,
        m_window_input_tracker.IsKeyToggled(KeyButtons::F),
        m_b_active_viewport_hovered,
        m_active_input_viewport_id,
        m_free_look_capture_viewport_id
    );

    m_window_input_snapshot = m_window_input_tracker.Finalize(WindowInputPolicy{
        .scene_active =
            m_b_scene_color_mouse_captured || m_free_look_capture_viewport_id != 0,
        .viewport_hovered = m_b_active_viewport_hovered,
        .viewport_resolution = uint2(
            to_extent_component(m_scene_color_resolution.x),
            to_extent_component(m_scene_color_resolution.y)
        ),
        .viewport_position = uint2(
            to_extent_component(m_scene_color_pos.x),
            to_extent_component(m_scene_color_pos.y)
        ),
    });

    if (!any_mouse_button_down) {
        // Keep capture ownership through the release frame so a drag that
        // leaves the viewport still publishes its paired release edge.
        m_b_scene_color_mouse_captured = false;
    }

    const uint32 main_viewport_id = ImGui::GetMainViewport()->ID;
    const uint32 cursor_viewport_id =
        m_active_input_viewport_id != 0 ? m_active_input_viewport_id : main_viewport_id;
    const auto resolve_platform_window = [](uint32 viewport_id) -> WindowType* {
        ImGuiViewport* viewport = ImGui::FindViewportByID(viewport_id);
        return viewport != nullptr ? static_cast<WindowType*>(viewport->PlatformHandle) : nullptr;
    };

    if (m_cursor_capture_viewport_id != 0 &&
        (m_cursor_capture_viewport_id != cursor_viewport_id ||
         !m_window_input_snapshot.cursor_hidden)) {
        if (WindowType* previous_window =
                resolve_platform_window(m_cursor_capture_viewport_id)) {
            WindowHandle previous_handle{previous_window};
            WindowContext::ApplyCursorMode(&previous_handle, false);
        }
        m_cursor_capture_viewport_id = 0;
    }
    if (m_window_input_snapshot.cursor_hidden) {
        if (WindowType* cursor_window = resolve_platform_window(cursor_viewport_id)) {
            WindowHandle active_handle{cursor_window};
            WindowContext::ApplyCursorMode(&active_handle, true);
            m_cursor_capture_viewport_id = cursor_viewport_id;
        }
    }

    m_ui_renderer->EndGUIFrame();
    m_ui_renderer->UpdatePlatformWindows();
}

void EditorUI::ShowSceneEditing(Scene& scene) {
    m_scene_editing_ui.ShowWindow(&m_b_show_scene_editing, scene);
}

void EditorUI::ShowFileMenu(Scene& scene, bool is_scene_loading) {
    const std::string file_menu_label = BuildFileMenuLabel(m_config->scene_path);
    if (!ImGui::BeginMenu(file_menu_label.c_str())) {
        return;
    }

    if (ImGui::MenuItem("Open Scene...", nullptr, false, !is_scene_loading)) {
        std::string selected_path;
        switch (OpenSceneFileDialog(selected_path)) {
            case ESceneFileDialogResult::Selected:
                LOG_INFO("User selected file: {}", selected_path);
                m_b_need_reload           = true;
                m_config->scene_path      = std::move(selected_path);
                m_last_file_action_status = "Open Scene requested";
                break;
            case ESceneFileDialogResult::Cancelled:
                LOG_INFO("User pressed cancel.");
                break;
            case ESceneFileDialogResult::Error:
                m_last_file_action_status = "Open Scene failed. Check log.";
                break;
        }
    }

    if (ImGui::MenuItem("Import Into Current Scene...", nullptr, false, scene.IsReady())) {
        std::string selected_path;
        if (OpenSceneFileDialog(selected_path) == ESceneFileDialogResult::Selected) {
            const Scene::ImportSceneFromFileResult result = scene.ImportSceneFromFileSync(selected_path);
            if (result) {
                m_last_file_action_status =
                    "Imported scene entities: " + std::to_string(result.imported_entity_count);
            } else {
                m_last_file_action_status = "Import failed: " + result.error_message;
            }
        }
    }

    if (ImGui::BeginMenu("Scene Cache")) {
        if (ImGui::MenuItem("Save State Cache")) {
            if (scene.SaveStateCache()) {
                m_last_file_action_status = "State cache saved";
            } else {
                m_last_file_action_status = "Save State failed. Check log.";
            }
        }

        const bool can_restore_source_scene = scene.IsReady() && !scene.GetSourceFilePath().empty();

        if (ImGui::MenuItem("Load State Cache", nullptr, false, can_restore_source_scene)) {
            m_b_need_reload           = true;
            m_last_file_action_status = "Load Cache requested";
        }

        if (ImGui::MenuItem("Reset To Source Scene", nullptr, false, can_restore_source_scene)) {
            if (scene.ResetToSourceScene()) {
                m_b_need_reload           = true;
                m_last_file_action_status = "Reset Cache requested";
            } else {
                m_last_file_action_status = "Reset failed. Check log.";
            }
        }

        ImGui::EndMenu();
    }

    ImGui::Separator();
    if (ImGui::MenuItem("Exit")) {
        WindowContext::RequestClose(WindowContext::GetMainWindow());
    }
    ImGui::EndMenu();
}

void EditorUI::ShowHierarchy(Scene& scene) {
    if (!m_b_show_hierarchy) {
        return;
    }

    if (!ImGui::Begin("Hierarchy", &m_b_show_hierarchy)) {
        ImGui::End();
        return;
    }

    if (!scene.IsReady()) {
        ImGui::TextDisabled("scene加载中");
        ImGui::End();
        return;
    }

    if (!scene.IsValidNodeEntity(m_selected_node)) {
        m_selected_node = entt::null;
    }

    const entt::entity root_entt = scene.GetRootNodeEntity();
    if (!scene.IsValidNodeEntity(root_entt)) {
        ImGui::TextDisabled("Root node not found.");
        ImGui::End();
        return;
    }

    const auto draw_node = [&](auto& self, entt::entity entity) -> void {
        const std::string           label        = scene.GetNodeDisplayName(entity);
        const Scene::NodeVisibility visibility   = scene.GetNodeVisibility(entity);
        const bool                  selected     = (m_selected_node == entity);
        const bool                  has_children = scene.GetNodeChildCount(entity) > 0;
        ImGuiTreeNodeFlags          tree_flags   = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow;

        if (selected) {
            tree_flags |= ImGuiTreeNodeFlags_Selected;
        }
        if (!has_children) {
            tree_flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        }

        ImGui::PushID(static_cast<int>(entt::to_integral(entity)));
        bool visible_in_game = visibility.visible_in_game;
        if (ImGui::Checkbox("##visible_game", &visible_in_game)) {
            scene.SetNodeVisibleInGame(entity, visible_in_game);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                visibility.effectively_visible_in_game ? "Visible in Game" : "Hidden in Game"
            );
        }
        ImGui::SameLine();
        ImGui::SetNextItemOpen(false, ImGuiCond_Once);
        const bool dim_node_label = !visibility.effectively_visible_in_game;
        if (dim_node_label) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        }
        const bool is_open = ImGui::TreeNodeEx(label.c_str(), tree_flags);
        if (dim_node_label) {
            ImGui::PopStyleColor();
        }
        if (ImGui::IsItemClicked()) {
            m_selected_node = entity;
        }

        if (has_children && is_open) {
            scene.ForEachNodeChild(entity, [&](entt::entity child_entt) {
                self(self, child_entt);
            });
            ImGui::TreePop();
        }
        ImGui::PopID();
    };

    scene.ForEachNodeChild(root_entt, [&](entt::entity child_entt) {
        draw_node(draw_node, child_entt);
    });

    ImGui::End();
}

void EditorUI::ShowInspector(Scene& scene) {
    m_inspector_ui.ShowWindow(&m_b_show_inspector, &scene, m_selected_node);
}

Render::UiDrawFramePacket EditorUI::CaptureDrawFrame() {
    return m_ui_renderer->CaptureDrawFrame();
}

bool EditorUI::IsSeparateWindow() const {
    auto* current_window = ImGui::FindWindowByName(GetActiveViewportWindowName());
    if (!current_window) {
        return false;
    }
    return current_window->ParentWindow == nullptr;
}

TextureRef EditorUI::GetWindowFrameBuffer() {
    auto* current_window = ImGui::FindWindowByName(GetActiveViewportWindowName());
    if (current_window && current_window->ParentWindow == nullptr && current_window->Viewport) {
        return m_ui_renderer->GetWindowFrameBuffer(current_window->Viewport);
    }
    return TextureRef();
}

const char* EditorUI::GetActiveViewportWindowName() const {
    return m_config->active_viewport_mode == EEditorViewportMode::Scene ? "Scene" : "Game";
}

void EditorUI::ShowGameView() {
    ShowViewportWindow("Game", &m_b_show_scene_color, EEditorViewportMode::Game);
}

void EditorUI::ShowSceneView(Scene&) {
    ShowViewportWindow("Scene", &m_b_show_scene_view, EEditorViewportMode::Scene);
}

void EditorUI::ShowViewportWindow(
    const char*         window_name,
    bool*               p_open,
    EEditorViewportMode viewport_mode
) {
    const bool was_active_viewport = m_config->active_viewport_mode == viewport_mode;

    ImGuiWindowFlags window_flags = ImGuiWindowFlags_MenuBar;
    if (was_active_viewport) {
        window_flags |= ImGuiWindowFlags_NoBackground;
    }

    if (!p_open || !*p_open) {
        if (m_config->active_viewport_mode == viewport_mode) {
            m_b_scene_color_mouse_captured = false;
        }
        return;
    }

    if (!ImGui::Begin(window_name, p_open, window_flags)) {
        if (m_config->active_viewport_mode == viewport_mode) {
            m_b_scene_color_mouse_captured = false;
        }
        ImGui::End();
        return;
    }

    bool is_active_viewport = m_config->active_viewport_mode == viewport_mode;
    if (!is_active_viewport && !m_b_active_viewport_window_seen) {
        m_config->active_viewport_mode = viewport_mode;
        is_active_viewport             = true;
    }
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        m_config->active_viewport_mode = viewport_mode;
        is_active_viewport             = true;
    }

    if (ImGui::BeginMenuBar()) {
        ImGui::TextDisabled("%s", is_active_viewport ? "Active" : "Inactive");
        ImGui::TextDisabled(
            "%s",
            viewport_mode == EEditorViewportMode::Game ? "Main Camera" : "Free Camera"
        );
        if (viewport_mode == EEditorViewportMode::Scene) {
            auto& gizmos = m_config->scene_view_gizmos;
            ImGui::Separator();
            ImGui::MenuItem("Gizmos", nullptr, &gizmos.enabled);
            if (ImGui::BeginMenu("Gizmo Layers")) {
                ImGui::Checkbox("Main Camera", &gizmos.show_main_camera);
                ImGui::Separator();

                if (ImGui::BeginMenu("CSM")) {
                    ImGui::Checkbox("Enabled", &gizmos.show_csm);
                    ImGui::BeginDisabled(!gizmos.show_csm);
                    ImGui::Checkbox("Split Frustums", &gizmos.show_csm_split_frustums);
                    ImGui::Checkbox("Bounding Spheres", &gizmos.show_csm_bounding_spheres);

                    if (ImGui::BeginMenu("Cascades")) {
                        const uint active_cascade_count = Min(
                            static_cast<uint>(m_config->raster_config.shadow_csm_num_of_cascades),
                            static_cast<uint>(CSM_MAX_CASCADES)
                        );
                        for (uint cascade_index = 0u; cascade_index < CSM_MAX_CASCADES; ++cascade_index) {
                            ImGui::PushID(static_cast<int>(cascade_index));
                            ImGui::BeginDisabled(cascade_index >= active_cascade_count);

                            const float4 color = GetCsmGizmoCascadeColor(cascade_index);
                            ImGui::ColorButton(
                                "##Color",
                                ImVec4(color.x, color.y, color.z, color.w),
                                ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                                ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight())
                            );
                            ImGui::SameLine();

                            bool cascade_visible =
                                (gizmos.csm_cascade_visibility_mask & (1u << cascade_index)) != 0u;
                            const std::string cascade_label = "Cascade " + std::to_string(cascade_index);
                            if (ImGui::Checkbox(cascade_label.c_str(), &cascade_visible)) {
                                if (cascade_visible) {
                                    gizmos.csm_cascade_visibility_mask |= 1u << cascade_index;
                                } else {
                                    gizmos.csm_cascade_visibility_mask &= ~(1u << cascade_index);
                                }
                            }

                            ImGui::EndDisabled();
                            ImGui::PopID();
                        }
                        ImGui::EndMenu();
                    }

                    ImGui::EndDisabled();
                    ImGui::EndMenu();
                }
                ImGui::Separator();

                ImGui::Checkbox("Probe GI", &gizmos.show_probe_gi);
                ImGui::BeginDisabled(!gizmos.show_probe_gi);
                ImGui::Checkbox("Probe Points", &gizmos.show_probe_gi_probes);
                ImGui::Checkbox("Probe Volumes", &gizmos.show_probe_gi_volume_bounds);
                ImGui::Checkbox("Adaptive Cells", &gizmos.show_probe_gi_adaptive_cells);
                ImGui::EndDisabled();
                ImGui::Separator();

                ImGui::SliderFloat("Line Thickness", &gizmos.line_thickness, 0.001f, 0.02f);
                ImGui::EndMenu();
            }
        }
        ImGui::EndMenuBar();
    }

    if (!is_active_viewport) {
        ImGui::End();
        return;
    }

    auto* current_window = ImGui::FindWindowByName(window_name);
    if (!current_window) {
        ImGui::End();
        return;
    }

    m_b_active_viewport_window_seen = true;
    if (current_window->Viewport != nullptr) {
        m_active_input_viewport_id = current_window->Viewport->ID;
    }

    const bool separate_window = current_window->ParentWindow == nullptr;
    const auto menu_rect       = current_window->MenuBarRect();
    const auto menu_bar_height = current_window->MenuBarHeight();

    const float2 scene_size = {
        current_window->Size.x,
        current_window->Size.y + current_window->Pos.y - menu_rect.Max.y
    };

    const auto window_rect = current_window->Rect();
    ImRect     parent_rect{};

    if (separate_window) {
        parent_rect = {
            current_window->Pos.x,
            current_window->Pos.y,
            current_window->Pos.x + current_window->Size.x,
            current_window->Pos.y + current_window->Size.y
        };

        const float2 local_pos = {window_rect.Min.x - parent_rect.Min.x, menu_rect.Max.y - parent_rect.Min.y};
        m_scene_color_resolution = {scene_size.x, scene_size.y};
        m_scene_color_pos        = {local_pos.x, local_pos.y};
    } else {
        parent_rect = current_window->ParentWindow->Rect();

        const float2 local_pos = {
            window_rect.Min.x - parent_rect.Min.x,
            menu_rect.Max.y + menu_bar_height - parent_rect.Min.y
        };

        m_scene_color_resolution = {scene_size.x, scene_size.y};
        m_scene_color_pos        = {local_pos.x, local_pos.y};
    }

    // 只有从渲染视口内部开始的拖拽才会控制摄像机。
    static constexpr float k_viewport_border = 4.f;

    const float  scene_color_top = menu_rect.Max.y + (separate_window ? 0.f : menu_bar_height);
    const ImVec2 scene_color_min =
        ImVec2(window_rect.Min.x + k_viewport_border, scene_color_top + k_viewport_border);
    const ImVec2 scene_color_max = ImVec2(
        window_rect.Min.x + m_scene_color_resolution.x - k_viewport_border,
        scene_color_top + m_scene_color_resolution.y - k_viewport_border
    );
    const WindowInputSourceSnapshot& input_source = m_window_input_tracker.GetPendingSource();
    const ImVec2 mouse_pos(input_source.mouse_position.x, input_source.mouse_position.y);

    const bool scene_color_hovered =
        ImGui::IsWindowHovered() &&
        mouse_pos.x > scene_color_min.x && mouse_pos.x < scene_color_max.x &&
        mouse_pos.y > scene_color_min.y && mouse_pos.y < scene_color_max.y;
    m_b_active_viewport_hovered = scene_color_hovered;

    const bool mouse_clicked = input_source.mouse_button_pressed[MouseButtons::Left] ||
                               input_source.mouse_button_pressed[MouseButtons::Middle] ||
                               input_source.mouse_button_pressed[MouseButtons::Right];
    if (!m_b_scene_color_mouse_captured && mouse_clicked && scene_color_hovered) {
        m_b_scene_color_mouse_captured = true;
    }

    ImGui::End();
}

void EditorUI::ShowConfig(Scene& scene) {
    ImGuiIO&         io           = ImGui::GetIO();
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_None;

    if (!m_b_show_config) {
        return;
    }
    if (!ImGui::Begin("Configs", &m_b_show_config, window_flags)) {
        ImGui::End();
        return;
    }

    ImGui::PushItemWidth(120); // 设置所有组件width为120

    EditorUIStyle::ShowStyleSelector("Style##Default");

    // MARK: Common Configs

    /////////////////////////////////////////////////// Begin Disabled Here
    // 避免场景加载一半，切换场景或渲染器，导致崩溃
    const bool is_scene_loading = !scene.IsReady() && scene.IsStartLoading();
    ImGui::BeginDisabled(is_scene_loading);

    if (!m_last_file_action_status.empty()) {
        ImGui::TextDisabled("File Action: %s", m_last_file_action_status.c_str());
    }

    if (ImGui::TreeNode("Camera")) {
        ImGui::SliderFloat("Speed (log10)", &m_config->camera_speed_log10, -1.f, 2.6f);

        ImGui::Checkbox("Override Projection", &m_config->camera_projection_override_enabled);

        if (!m_config->camera_projection_override_enabled) {
            ImGui::TextDisabled("Using scene camera projection.");
        }

        ImGui::BeginDisabled(!m_config->camera_projection_override_enabled);
        ImGui::SliderFloat("Fov Y", &m_config->camera_fovy, 1.f, 160.f);

        ImGui::SliderFloat("Near Clip (log10)", &m_config->camera_near_clip_log10, -4.f, 0.99f);
        ImGui::SliderFloat("Far Clip (log10)", &m_config->camera_far_clip_log10, 0.f, 4.f);
        m_config->camera_near_clip_log10 =
            std::min(m_config->camera_near_clip_log10, m_config->camera_far_clip_log10 - 0.1f);
        ImGui::EndDisabled();

        ImGui::TreePop();
    }

    const bool remote_enabled = m_remote_controller.IsEnabled();
    if (ImGui::TreeNode("Remote", "Remote Module: [%s]", remote_enabled ? "Running" : "Closed")) {
        ImGui::BeginDisabled(!m_remote_controller.IsValid());
        if (ImGui::Button(remote_enabled ? "Disable" : "Enable")) {
            m_remote_controller.SetEnabled(!remote_enabled);
        }
        ImGui::EndDisabled();

        if (remote_enabled) {
            const auto remote_config = m_remote_controller.GetConfigSnapshot();

            ImGui::Text("Bind Address: %s", remote_config.bind_address.c_str());
            ImGui::Text("HTTP Port: %u", remote_config.http_port);
            ImGui::Text("WebSocket Port: %u", remote_config.websocket_port);

            if (ImGui::Button("Open Remote Examples")) {
                m_remote_examples_ui.Open();
            }
        } else {
            m_remote_examples_ui.Close();
        }

        ImGui::TreePop();
    }

    if (m_b_show_render_config_sub_ui) {
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
    if (is_scene_loading) {
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

void EditorUI::ResetFrameState() {
    m_b_need_reload = false;
}

// 新增顶层 Sub UI 时，需要同步更新 EditorUISettings.h 中记录的持久化路径
void EditorUI::SyncWindowVisibilitySettings() {
    EditorWindowVisibilitySettings settings;
    settings.loaded        = true;
    settings.scene_color   = m_b_show_scene_color;
    settings.scene_view    = m_b_show_scene_view;
    settings.hierarchy     = m_b_show_hierarchy;
    settings.inspector     = m_b_show_inspector;
    settings.config        = m_b_show_config;
    settings.scene_editing = m_b_show_scene_editing;
    settings.profile_capture = m_b_show_profile_capture;
    settings.profile_viewer = m_b_show_profile_viewer;
#if WITH_PROFILE
    settings.memory_profiler = m_b_show_memory_profiler;
#else
    settings.memory_profiler = EditorUISettings::GetWindowVisibilitySettings().memory_profiler;
#endif

    EditorUISettings::StoreWindowVisibilitySettings(settings);
}

void EditorUI::RegisterUIFunc(std::string name, std::function<void()>&& callback) {
    m_show_func_map[std::move(name)] = std::move(callback);
}

void EditorUI::UnregisterUIFunc(std::string name) {
    m_show_func_map.erase(name);
}

} // namespace Moer
