#pragma once

#include <array>
#include <deque>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <imgui.h>

#include "Core.h"
#include "renderer/EditorConfig.h"

namespace Moer {

class ConsoleSystem {
public:
    ConsoleSystem(const SharedPtr<EditorConfig>& editor_config);

    void ExecuteCommand(std::string_view line);
    void TickUI();

private:
    enum class EDisplayMode {
        Hidden = 0,
        Inline,
        Windowed,
    };

    void PumpRuntimeLogs();
    void HandleHotkeys();
    void CycleMode();
    void ToggleEditorConsolePage();
    bool IsRenderWindowActive() const;
    bool IsRenderConsoleVisible() const;

    void AppendOutput(std::string line);
    void DrawEditorConsolePanel();
    void DrawRenderInlineConsole();
    void DrawRenderWindowedConsole();
    void DrawOutputPanel(const char* id, float input_height);
    void DrawInputLine(const char* id);
    void UpdateAutocompleteCandidates();
    void UpdateAutocompleteCandidatesForInput(std::string_view raw_input);
    void DrawAutocompleteList(const char* id);
    void AcceptAutocompleteSelection();
    std::string GetSelectedAutocompleteText() const;
    void NavigateHistory(int delta);
    void SetInputBuffer(std::string_view text);
    float GetAutocompletePanelHeight() const;
    int  OnInputTextCallback(ImGuiInputTextCallbackData* data);
    bool IsEditorConsoleInteracting() const;

    SharedPtr<EditorConfig> m_editor_config;

    std::deque<std::string> m_output_lines;
    std::vector<std::string> m_command_history;

    std::array<char, 512> m_input_buffer{};

    EDisplayMode m_mode = EDisplayMode::Hidden;

    bool m_editor_console_page_open = true;
    bool m_editor_console_input_active   = false;

    bool m_last_grave_switch_state = false;
    bool m_last_escape_down        = false;
    bool m_focus_input_next_frame  = false;
    bool m_focus_window_next_frame = false;
    int  m_focus_input_frames      = 0;

    uint64_t m_next_log_sequence = 1;

    bool m_auto_scroll = true;

    std::vector<std::string> m_autocomplete_candidates;
    int                      m_autocomplete_selected = -1;
    std::string              m_last_autocomplete_query;

    int         m_history_cursor = -1;
    std::string m_history_backup;

    static constexpr size_t k_max_output_lines = 1024;
};

} // namespace Moer
