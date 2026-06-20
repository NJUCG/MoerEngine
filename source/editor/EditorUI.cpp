#include "EditorUI.h"

// Runtime
#include "file/FileDialog.h"
#include "log/LogSystem.h"
#include "string/StringConvert.h"
#include "trace/Trace.h"
#include "window/WindowInput.h"

// Editor
#include "scene/Scene.h"
#include "scene/SceneGlobalEntry.h"

// 3rd party (std)
#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <string_view>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

using namespace Moer::Render;

namespace Moer {
namespace {

bool AnyCameraMouseButtonDown(const Render::ImGuiIOInputSnapshot& input) {
    return input.mouse_button_down[MouseButtons::Left] || input.mouse_button_down[MouseButtons::Middle] ||
           input.mouse_button_down[MouseButtons::Right];
}

void ClearCameraInput(WindowInput& input) {
    input.camera_forward  = false;
    input.camera_backward = false;
    input.camera_left     = false;
    input.camera_right    = false;
    input.camera_up       = false;
    input.camera_down     = false;
    input.speed_up        = false;
    input.speed_down      = false;
    input.reset_speed     = false;
}

void StoreSelectedPlatformPath(StringView selected_path, void* user_data) {
    *static_cast<String*>(user_data) = String(selected_path);
}

bool LaunchProfilerProcess(const std::filesystem::path& capture_path = {}) {
#if defined(_WIN32)
    PlatformChar module_path[MAX_PATH]{};
    const DWORD module_length = GetModuleFileNameW(nullptr, module_path, static_cast<DWORD>(std::size(module_path)));
    if (module_length == 0 || module_length >= std::size(module_path)) {
        LOG_WARNING(MOER_TEXT("Failed to resolve MoerEditor executable path."));
        return false;
    }

    const std::filesystem::path editor_path   = std::filesystem::path(module_path);
    const std::filesystem::path profiler_path = editor_path.parent_path() / std::filesystem::path(MOER_TEXT("MoerProfiler.exe"));
    String parameters{};
    if (!capture_path.empty()) {
        const String capture_path_text = String(capture_path.native());
        parameters.reserve(capture_path_text.size() + 2);
        parameters.push_back(MOER_TEXT('"'));
        parameters += capture_path_text;
        parameters.push_back(MOER_TEXT('"'));
    }
    HINSTANCE result = ShellExecuteW(
        nullptr,
        MOER_TEXT("open"),
        profiler_path.c_str(),
        parameters.empty() ? nullptr : parameters.c_str(),
        profiler_path.parent_path().c_str(),
        SW_SHOWNORMAL
    );
    if (reinterpret_cast<intptr_t>(result) <= 32) {
        LOG_WARNING(MOER_TEXT("Failed to launch MoerProfiler: {}"), String(profiler_path.native()));
        return false;
    }
    return true;
#else
    (void)capture_path;
    LOG_WARNING(MOER_TEXT("Launching MoerProfiler from Editor is only implemented on Windows."));
    return false;
#endif
}

void OpenProfilerCapturePicker() {
    static constexpr std::array<FileDialog::Filter, 3> capture_filters = {{
        {MOER_ASCII_TEXT("Profiler Capture"), MOER_ASCII_TEXT("mpd,mrtc,csv,bin")},
        {MOER_ASCII_TEXT("Trace CSV"), MOER_ASCII_TEXT("csv")},
        {MOER_ASCII_TEXT("All Files"), MOER_ASCII_TEXT("*")},
    }};

    String selected_path{};
    const FileDialog::EOpenFileStatus result = FileDialog::OpenFile(FileDialog::OpenFileRequest{
        .filters = capture_filters,
        .callback = StoreSelectedPlatformPath,
        .user_data = &selected_path,
    });
    if (result == FileDialog::EOpenFileStatus::Success) {
        LaunchProfilerProcess(std::filesystem::path(selected_path.Native()));
    } else if (result == FileDialog::EOpenFileStatus::Error) {
        LOG_WARNING(MOER_TEXT("Failed to open profiler capture picker."));
    }
}

} // namespace

EditorUI::EditorUI(UniquePtr<Render::UIRenderer> renderer, SharedPtr<EditorConfig> editor_config) :
    m_ui_renderer(std::move(renderer)),
    m_config(editor_config) {
    m_synapse_context = MakeUnique<Synapse::Context>();
    m_synapse_context->ApplyDefaultTheme();
}

void EditorUI::TickUI() {
    m_scene_color_hovered = false;
    m_scene_color_focused = false;

    if (m_config->play_mode_enabled) {
        if (WindowInput::Get().native_key_pressed[KeyButtons::F8]) {
            m_config->play_mode_enabled       = false;
            m_config->play_mode_capture_input = false;
            WindowInput::Get().force_cursor_hidden = false;
            WindowInput::Get().play_mode_camera_control = false;
            m_scene_color_input_active = false;
            m_scene_color_focused = false;
        } else {
            WindowInput& input          = WindowInput::Get();
            m_scene_color_pos          = {0.0f, 0.0f};
            m_scene_color_resolution   = {input.width, input.height};
        // m_config->aspect_ratio = 1.0f;  // aspect_ratio removed from EditorConfig
            m_scene_color_hovered      = true;
            m_scene_color_focused      = true;
            ApplyPlayInput();
            return;
        }
    }

    // 注：Resolution表示整个窗口的大小（不包含windows标题栏）；SceneColor只表示场景渲染区域的大小
    // 更新SceneColor的分辨率
    // m_config->aspect_ratio = 1.0f;  // aspect_ratio removed from EditorConfig

    m_ui_renderer->BeginGUIFrame();
    m_synapse_context->BeginFrame(m_ui_renderer->GetInputSnapshot());
    Synapse::Context& ui = *m_synapse_context;

    const Render::ImGuiIOInputSnapshot& input_snapshot = ui.GetInputSnapshot();

    if (input_snapshot.f5_pressed && !m_config->play_mode_enabled) {
        m_config->play_mode_enabled       = true;
        m_config->play_mode_capture_input = true;
    }

    if (ui.BeginMainDockspace(Synapse::DockspaceDesc{
            .open            = &m_b_show,
#if WITH_PROFILE
            .enable_profiler = m_runtime_profiler.IsOpen(),
#else
            .enable_profiler = false,
#endif
        })) {
        if (ui.BeginMenuBar()) {
            if (ui.BeginMenu("Menu")) {
                if (ui.MenuItem("Exit")) {
                    exit(0);
                }
                ui.EndMenu();
            }
            if (ui.BeginMenu("Window")) {

                ui.MenuItem("Scene Color", &m_b_show_scene_color);
                ui.MenuItem("Configs", &m_b_show_config);
                if (m_get_console_window_visible && m_set_console_window_visible) {
                    bool show_console = m_get_console_window_visible();
                    if (ui.MenuItem("Console", &show_console)) {
                        m_set_console_window_visible(show_console);
                    }
                }
#if WITH_PROFILE
                bool show_runtime_profiler = m_runtime_profiler.IsOpen();
                if (ui.MenuItem("runtime_profiler", &show_runtime_profiler)) {
                    m_runtime_profiler.SetOpen(show_runtime_profiler);
                }
#endif
                ui.EndMenu();
            }
            ui.Separator();
            if (!m_config->play_mode_enabled) {
                if (ui.IconButton(Synapse::EIcon::Play, "Play (F5)")) {
                    m_config->play_mode_enabled       = true;
                    m_config->play_mode_capture_input = true;
                }
            } else {
                if (ui.IconButton(Synapse::EIcon::Stop, "Stop Play")) {
                    m_config->play_mode_enabled       = false;
                    m_config->play_mode_capture_input = false;
                }
            }
            ui.SameLine();
            const Trace::Stats trace_stats = Trace::GetStats();
            if (ui.IconButton(
                    Synapse::EIcon::Record,
                    trace_stats.recording ? "Stop Trace" : "Start Trace",
                    trace_stats.recording
                )) {
                if (trace_stats.recording) {
                    Trace::StopRecording();
                } else {
                    Trace::StartRecording();
                }
            }
            ui.SameLine();
            if (ui.IconButton(Synapse::EIcon::Profiler, "Open MoerProfiler")) {
                ui.OpenPopup("EditorProfilerLaunchPopup");
            }
            if (ui.BeginPopup("EditorProfilerLaunchPopup")) {
                if (ui.MenuItem("Open Profiler")) {
                    LaunchProfilerProcess();
                }

                const Utf8String trace_csv_path = Trace::GetCsvPath();
                ui.BeginDisabled(trace_csv_path.empty());
                if (ui.MenuItem("Open Current Trace")) {
                    LaunchProfilerProcess(std::filesystem::path(trace_csv_path.c_str()));
                }
                ui.EndDisabled();

                if (ui.MenuItem("Open Trace...")) {
                    OpenProfilerCapturePicker();
                }
                ui.EndPopup();
            }
            ui.EndMenuBar();
        }
    }
    ui.EndMainDockspace();
    ResetState();
    ShowSceneColor();
    ShowConfig();
#if WITH_PROFILE
    m_runtime_profiler.TickUI();
#endif
    ShowOverlay();

    ApplyInputSnapshot();
    m_synapse_context->EndFrame();
    m_ui_renderer->EndGUIFrame();
}

void EditorUI::RenderGUI(Render::CommandList& cmd_list, const Render::TextureView& final_output) {
    m_ui_renderer->RenderGUI(cmd_list, final_output);
}

void EditorUI::PresentWindows() {
    m_ui_renderer->PresentWindows();
}

void EditorUI::PublishSceneRenderOutput(Render::TextureView resource) {
    m_ui_renderer->PublishRenderOutput(m_scene_render_output_slot, resource);
}

void EditorUI::ShowSceneColor() {
    if (!m_b_show_scene_color) {
        m_scene_color_input_active = false;
        m_scene_color_focused = false;
        return;
    }
    const Synapse::SceneViewportState scene_view =
        m_synapse_context->DrawSceneViewportPanel("Scene Color", &m_b_show_scene_color, *m_ui_renderer);
    if (!scene_view.visible) {
        m_scene_color_input_active = false;
        m_scene_color_focused = false;
        return;
    }
    m_scene_color_resolution = scene_view.content_resolution;
    m_scene_color_pos        = scene_view.content_pos;
    m_scene_color_hovered    = scene_view.hovered;
    m_scene_color_focused    = scene_view.focused;
    if (scene_view.input_started) {
        m_scene_color_input_active = true;
    }
    if (!scene_view.focused || !scene_view.mouse_down) {
        m_scene_color_input_active = false;
    }
    m_scene_render_output_slot = scene_view.render_output_slot;
}

void EditorUI::ShowConfig() {
    Synapse::Context& ui = *m_synapse_context;

    if (!m_b_show_config) {
        return;
    }
    if (!ui.BeginPanel(Synapse::PanelDesc{.name = "Configs", .open = &m_b_show_config})) {
        return;
    }

    ui.PushItemWidth(ui.GetTheme().item_width);

    if (ui.TreeNode("Trace")) {
        const Moer::Trace::Stats trace_stats = Moer::Trace::GetStats();
        ui.Text("Enabled: %s", trace_stats.enabled ? "Yes" : "No");
        ui.Text("Recording: %s", trace_stats.recording ? "On" : "Off");
        if (trace_stats.recording) {
            if (ui.Button("Stop Trace")) {
                Moer::Trace::StopRecording();
            }
        } else {
            if (ui.Button("Start Trace")) {
                Moer::Trace::StartRecording();
            }
        }
        ui.Text("Connected: %s", trace_stats.connected ? "Yes" : "No");
        ui.Text("Queued Events: %llu", static_cast<unsigned long long>(trace_stats.queued_events));
        ui.Text("Dropped Events: %llu", static_cast<unsigned long long>(trace_stats.dropped_events));
        ui.TreePop();
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
    ui.BeginDisabled(is_scene_found_but_not_ready);

    // Render Method
    {
        auto last_selected_render_method = m_config->selected_render_method;
        if (ui.BeginCombo(
                "Render Method",
                k_render_method_names[static_cast<uint>(m_config->selected_render_method)].data()
            )) {
            for (int i = 0; i < static_cast<int>(std::size(k_render_method_names)); i++) {
                const bool is_selected = (m_config->selected_render_method == static_cast<ERenderMethod>(i));
                if (ui.Selectable(k_render_method_names[i].data(), is_selected)) {
                    m_config->selected_render_method = static_cast<ERenderMethod>(i);
                }
                if (is_selected) {
                    ui.SetItemDefaultFocus();
                }
            }
            ui.EndCombo();
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
        if (ui.Button("Open Scene")) {
            static constexpr std::array<FileDialog::Filter, 5> scene_filters = {{{
                MOER_ASCII_TEXT("All Support Formats"),
                MOER_ASCII_TEXT("glb,gltf,fbx,obj,dae")
            }, {
                MOER_ASCII_TEXT("glTF 2.0"),
                MOER_ASCII_TEXT("glb,gltf")
            }, {
                MOER_ASCII_TEXT("FBX"),
                MOER_ASCII_TEXT("fbx")
            }, {
                MOER_ASCII_TEXT("Wavefront"),
                MOER_ASCII_TEXT("obj")
            }, {
                MOER_ASCII_TEXT("Moer Renderer Scene (WIP)"),
                MOER_ASCII_TEXT("json")
            }}};
            String selected_scene_path{};
            const FileDialog::EOpenFileStatus result = FileDialog::OpenFile(FileDialog::OpenFileRequest{
                .filters = scene_filters,
                .callback = StoreSelectedPlatformPath,
                .user_data = &selected_scene_path,
            });
            if (result == FileDialog::EOpenFileStatus::Success) {
                LOG_INFO(MOER_TEXT("User selected file: {}"), selected_scene_path);

                // Prepare for reload
                m_b_need_reload      = true;
                m_config->scene_path = PlatformToUtf8(selected_scene_path).Native();
            } else if (result == FileDialog::EOpenFileStatus::Cancelled) {
                LOG_INFO(MOER_TEXT("User pressed cancel."));
            }
        }
        ui.SameLine();
        ui.Text("Current: [%s]", scene_name.c_str());
    }

    if (ui.TreeNode("Camera")) {

        ui.SliderFloat("Speed (log10)", &m_config->camera_speed_log10, -1.f, 2.6f);

        ui.Checkbox("Override Projection", &m_config->camera_projection_override_enabled);

        bool projection_changed = false;
        projection_changed |= ui.SliderFloat("Fov Y", &m_config->camera_fovy, 1.f, 160.f);
        projection_changed |=
            ui.SliderFloat("Near Clip (log10)", &m_config->camera_near_clip_log10, -4.f, 0.99f);
        projection_changed |=
            ui.SliderFloat("Far Clip (log10)", &m_config->camera_far_clip_log10, 0.f, 4.f);
        m_config->camera_near_clip_log10 =
            std::min(m_config->camera_near_clip_log10, m_config->camera_far_clip_log10 - 0.1f);

        if (projection_changed) {
            m_config->camera_projection_override_enabled = true;
        }

        ui.TreePop();
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

        ui.SeparatorText(active_renderer.c_str());
        for (const std::string& section_name : section_names) {
            auto section_iter = renderer_iter->second.find(section_name);
            if (section_iter == renderer_iter->second.end()) {
                continue;
            }
            if (ui.TreeNode(section_name.c_str())) {
                section_iter->second(ui);
                ui.TreePop();
            }
        }
    }

    ui.Separator();

    ui.EndDisabled();
    if (is_scene_found_but_not_ready) {
        ui.TextColored({1.0f, 0.4f, 0.4f, 1.0f}, "Scene is loading... Please wait.");
    }
    /////////////////////////////////////////////////// End Disabled Here

    const float framerate = ui.GetFramerate();
    ui.Text("Infos: ");
    ui.Text("\tFPS: %.1f", framerate);
    ui.Text("\tFrame Time: %.1f ms", 1000.0f / framerate);

    ui.PopItemWidth();

    ui.EndPanel();
}

void EditorUI::ResetState() {
    m_b_need_reload = false;
}

void EditorUI::RegisterRendererConfigSection(
    std::string renderer_name,
    std::string section_name,
    RendererConfigDrawFunc&& func
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
    const Render::ImGuiIOInputSnapshot& input_snapshot = m_ui_renderer->GetInputSnapshot();
    if (!AnyCameraMouseButtonDown(input_snapshot)) {
        m_scene_color_input_active = false;
    }
    const bool scene_camera_active = m_scene_color_focused && m_scene_color_input_active;

    Render::ApplyImGuiIOInputToWindowInput(
        input_snapshot,
        Render::ImGuiIOInputApplyParams{
            .scene_active            = scene_camera_active,
            .play_capture            = m_config->play_mode_enabled && m_config->play_mode_capture_input,
            .external_key_block      = false,
            .external_cursor_visible = false,
        }
    );
}

void EditorUI::ApplyPlayInput() {
    WindowInput& input = WindowInput::Get();

    input.is_active                = true;
    input.force_cursor_hidden      = true;
    input.force_cursor_visible     = false;
    input.play_mode_camera_control = true;
    input.block_camera_keyboard_input = false;

    input.cursor_last_x = input.native_mouse_pos.x;
    input.cursor_last_y = input.native_mouse_pos.y;
    if (input.is_cursor_dirty) {
        input.cursor_delta_x = 0.0f;
        input.cursor_delta_y = 0.0f;
        input.is_cursor_dirty = false;
    } else {
        input.cursor_delta_x = input.native_mouse_delta.x;
        input.cursor_delta_y = input.native_mouse_delta.y;
    }
    input.scroll_offset = 0.0f;

    for (uint32_t i = 0; i < MouseButtons::MouseButtonCount; ++i) {
        input.mouse_button_state[i] = input.native_mouse_button_down[i];
    }

    for (uint32_t i = 0; i < KeyButtons::KeyButtonCount; ++i) {
        input.key_button_state[i] = input.native_key_down[i];
        if (input.native_key_released[i]) {
            input.key_button_switch_state[i] = !input.key_button_switch_state[i];
        }
    }

    ClearCameraInput(input);
    input.camera_forward  = input.native_key_down[KeyButtons::W];
    input.camera_backward = input.native_key_down[KeyButtons::S];
    input.camera_left     = input.native_key_down[KeyButtons::A];
    input.camera_right    = input.native_key_down[KeyButtons::D];
    input.camera_up       = input.native_key_down[KeyButtons::E];
    input.camera_down     = input.native_key_down[KeyButtons::Q];
    input.speed_up        = input.native_key_down[KeyButtons::UP];
    input.speed_down      = input.native_key_down[KeyButtons::DOWN];
    input.reset_speed     = input.native_key_down[KeyButtons::F];
}

} // namespace Moer
