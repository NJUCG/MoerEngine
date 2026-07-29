#include "EditorUISettings.h"

// 在独立的 ImGui ini 配置段中持久化顶层编辑器窗口的可见性。

#include <imgui.h>
#include <imgui_internal.h>

#include <string_view>

namespace Moer {

namespace {

constexpr const char*      k_editor_window_visibility_type    = "EditorWindowVisibility";
constexpr const char*      k_editor_window_visibility_name    = "Main";
constexpr std::string_view k_editor_window_visibility_section = "[EditorWindowVisibility][Main]";

EditorWindowVisibilitySettings g_editor_window_visibility_settings;

std::string_view TrimIniLine(std::string_view line) {
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
        line.remove_prefix(1);
    }
    while (!line.empty() &&
           (line.back() == ' ' || line.back() == '\t' || line.back() == '\r' || line.back() == '\n')) {
        line.remove_suffix(1);
    }
    return line;
}

bool TryParseBoolSetting(std::string_view line, std::string_view key, bool& value) {
    if (line.size() <= key.size() || line.substr(0, key.size()) != key || line[key.size()] != '=') {
        return false;
    }

    const std::string_view value_text = TrimIniLine(line.substr(key.size() + 1));
    value                             = !value_text.empty() && value_text.front() != '0';
    return true;
}

void ParseEditorWindowVisibilityLine(EditorWindowVisibilitySettings& settings, std::string_view line) {
    line = TrimIniLine(line);
    if (line.empty() || line.front() == ';') {
        return;
    }

    if (TryParseBoolSetting(line, "SceneColor", settings.scene_color) ||
        TryParseBoolSetting(line, "SceneView", settings.scene_view) ||
        TryParseBoolSetting(line, "Hierarchy", settings.hierarchy) ||
        TryParseBoolSetting(line, "Inspector", settings.inspector) ||
        TryParseBoolSetting(line, "Configs", settings.config) ||
        TryParseBoolSetting(line, "SceneEditing", settings.scene_editing) ||
        TryParseBoolSetting(
            line,
            "ProfileCapture",
            settings.profile_capture
        ) ||
        TryParseBoolSetting(line, "MemoryProfiler", settings.memory_profiler)) {
        settings.loaded = true;
    }
}

void* EditorWindowVisibilitySettingsReadOpen(ImGuiContext*, ImGuiSettingsHandler*, const char* name) {
    if (std::string_view(name) != k_editor_window_visibility_name) {
        return nullptr;
    }

    g_editor_window_visibility_settings        = EditorWindowVisibilitySettings{};
    g_editor_window_visibility_settings.loaded = true;
    return &g_editor_window_visibility_settings;
}

void EditorWindowVisibilitySettingsReadLine(
    ImGuiContext*,
    ImGuiSettingsHandler*,
    void*       entry,
    const char* line
) {
    ParseEditorWindowVisibilityLine(*static_cast<EditorWindowVisibilitySettings*>(entry), line);
}

void EditorWindowVisibilitySettingsWriteAll(ImGuiContext*, ImGuiSettingsHandler*, ImGuiTextBuffer* out_buf) {
    const EditorWindowVisibilitySettings& settings = g_editor_window_visibility_settings;
    if (!settings.loaded) {
        return;
    }

    out_buf->appendf("[%s][%s]\n", k_editor_window_visibility_type, k_editor_window_visibility_name);
    out_buf->appendf("SceneColor=%d\n", settings.scene_color ? 1 : 0);
    out_buf->appendf("SceneView=%d\n", settings.scene_view ? 1 : 0);
    out_buf->appendf("Hierarchy=%d\n", settings.hierarchy ? 1 : 0);
    out_buf->appendf("Inspector=%d\n", settings.inspector ? 1 : 0);
    out_buf->appendf("Configs=%d\n", settings.config ? 1 : 0);
    out_buf->appendf("SceneEditing=%d\n", settings.scene_editing ? 1 : 0);
    out_buf->appendf(
        "ProfileCapture=%d\n",
        settings.profile_capture ? 1 : 0
    );
    out_buf->appendf("MemoryProfiler=%d\n\n", settings.memory_profiler ? 1 : 0);
}

void RegisterEditorWindowVisibilitySettingsHandler() {
    if (ImGui::FindSettingsHandler(k_editor_window_visibility_type) != nullptr) {
        return;
    }

    ImGuiSettingsHandler ini_handler;
    ini_handler.TypeName   = k_editor_window_visibility_type;
    ini_handler.TypeHash   = ImHashStr(k_editor_window_visibility_type);
    ini_handler.ReadOpenFn = EditorWindowVisibilitySettingsReadOpen;
    ini_handler.ReadLineFn = EditorWindowVisibilitySettingsReadLine;
    ini_handler.WriteAllFn = EditorWindowVisibilitySettingsWriteAll;
    ImGui::AddSettingsHandler(&ini_handler);
}

void LoadEditorWindowVisibilitySettingsFromLoadedIni() {
    g_editor_window_visibility_settings = EditorWindowVisibilitySettings{};

    ImGuiContext* context = ImGui::GetCurrentContext();
    if (context == nullptr) {
        return;
    }

    const ImGuiTextBuffer& settings_ini_data = context->SettingsIniData;
    if (settings_ini_data.Buf.Data == nullptr || settings_ini_data.Buf.Size <= 1) {
        return;
    }

    const std::string_view ini_data(
        settings_ini_data.Buf.Data, static_cast<size_t>(settings_ini_data.Buf.Size - 1)
    );
    size_t line_begin = ini_data.find(k_editor_window_visibility_section);
    if (line_begin == std::string_view::npos) {
        return;
    }

    line_begin += k_editor_window_visibility_section.size();
    g_editor_window_visibility_settings.loaded = true;

    while (line_begin < ini_data.size()) {
        const size_t line_end = ini_data.find('\n', line_begin);
        const size_t count =
            (line_end == std::string_view::npos) ? ini_data.size() - line_begin : line_end - line_begin;
        const std::string_view line = TrimIniLine(ini_data.substr(line_begin, count));
        if (!line.empty() && line.front() == '[') {
            break;
        }
        ParseEditorWindowVisibilityLine(g_editor_window_visibility_settings, line);

        if (line_end == std::string_view::npos) {
            break;
        }
        line_begin = line_end + 1;
    }
}

bool IsSameWindowVisibilitySettings(
    const EditorWindowVisibilitySettings& lhs,
    const EditorWindowVisibilitySettings& rhs
) {
    return lhs.loaded == rhs.loaded && lhs.scene_color == rhs.scene_color &&
           lhs.scene_view == rhs.scene_view && lhs.hierarchy == rhs.hierarchy &&
           lhs.inspector == rhs.inspector && lhs.config == rhs.config &&
           lhs.scene_editing == rhs.scene_editing &&
           lhs.profile_capture == rhs.profile_capture &&
           lhs.memory_profiler == rhs.memory_profiler;
}

} // namespace

namespace EditorUISettings {

const EditorWindowVisibilitySettings& LoadWindowVisibilitySettings() {
    RegisterEditorWindowVisibilitySettingsHandler();
    LoadEditorWindowVisibilitySettingsFromLoadedIni();
    return g_editor_window_visibility_settings;
}

const EditorWindowVisibilitySettings& GetWindowVisibilitySettings() {
    return g_editor_window_visibility_settings;
}

void StoreWindowVisibilitySettings(const EditorWindowVisibilitySettings& settings) {
    EditorWindowVisibilitySettings stored_settings = settings;
    stored_settings.loaded                         = true;

    if (!IsSameWindowVisibilitySettings(stored_settings, g_editor_window_visibility_settings)) {
        g_editor_window_visibility_settings = stored_settings;
        ImGui::MarkIniSettingsDirty();
    }
}

} // namespace EditorUISettings

} // namespace Moer
