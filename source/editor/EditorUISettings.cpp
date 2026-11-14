#include "EditorUISettings.h"

// 在独立的 ImGui ini 配置段中持久化顶层编辑器窗口的可见性。

#include <imgui.h>
#include <imgui_internal.h>

#include <string_view>
#include <utility>

namespace Moer {

namespace {

constexpr const char*      k_editor_window_visibility_type    = "EditorWindowVisibility";
constexpr const char*      k_editor_window_visibility_name    = "Main";
constexpr std::string_view k_editor_window_visibility_section = "[EditorWindowVisibility][Main]";

constexpr std::pair<const char*, bool EditorWindowVisibilitySettings::*> k_window_visibility_fields[] = {
    {"SceneColor", &EditorWindowVisibilitySettings::scene_color},
    {"SceneView", &EditorWindowVisibilitySettings::scene_view},
    {"Hierarchy", &EditorWindowVisibilitySettings::hierarchy},
    {"Inspector", &EditorWindowVisibilitySettings::inspector},
    {"Configs", &EditorWindowVisibilitySettings::config},
    {"SceneEditing", &EditorWindowVisibilitySettings::scene_editing},
    {"Console", &EditorWindowVisibilitySettings::console},
    {"ProfileCapture", &EditorWindowVisibilitySettings::profile_capture},
    {"ProfileViewer", &EditorWindowVisibilitySettings::profile_viewer},
    {"MemoryProfiler", &EditorWindowVisibilitySettings::memory_profiler},
};

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

    for (const auto& [key, member] : k_window_visibility_fields) {
        if (TryParseBoolSetting(line, key, settings.*member)) {
            settings.loaded = true;
            return;
        }
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
    for (const auto& [key, member] : k_window_visibility_fields) {
        out_buf->appendf("%s=%d\n", key, settings.*member ? 1 : 0);
    }
    out_buf->append("\n");
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

    if (stored_settings != g_editor_window_visibility_settings) {
        g_editor_window_visibility_settings = stored_settings;
        ImGui::MarkIniSettingsDirty();
    }
}

} // namespace EditorUISettings

} // namespace Moer
