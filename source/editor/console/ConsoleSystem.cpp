#include "console/ConsoleSystem.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include <imgui.h>
#include <imgui_internal.h>

#include "command/EngineCommandProcessor.h"
#include "log/LogSystem.h"
#include "renderer/EditorConsoleVariables.h"
#include "window/WindowInput.h"

namespace Moer {
namespace {
struct InputCallbackUserData {
    ConsoleSystem* console = nullptr;
};

struct PumpLogsContext {
    std::vector<std::string>* lines = nullptr;
};

struct PumpCommandOutputContext {
    std::vector<std::string>* lines = nullptr;
};

struct CollectAutocompleteContext {
    std::vector<ConsoleSystem::AutocompleteCandidate>* candidates = nullptr;
};

std::string Trim(std::string_view in) {
    size_t begin = 0;
    while (begin < in.size() && std::isspace(static_cast<unsigned char>(in[begin]))) {
        ++begin;
    }
    size_t end = in.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(in[end - 1]))) {
        --end;
    }
    return std::string(in.substr(begin, end - begin));
}

std::string LevelPrefix(spdlog::level::level_enum level) {
    switch (level) {
        case spdlog::level::trace:
            return "[Trace] ";
        case spdlog::level::debug:
            return "[Debug] ";
        case spdlog::level::info:
            return "[Info] ";
        case spdlog::level::warn:
            return "[Warn] ";
        case spdlog::level::err:
            return "[Error] ";
        case spdlog::level::critical:
            return "[Critical] ";
        default:
            return "[Log] ";
    }
}

void AppendRuntimeLogEntry(const LogSystem::ConsoleLogEntryView& entry, void* user_data) {
    auto* context = static_cast<PumpLogsContext*>(user_data);
    if (!context || !context->lines) {
        return;
    }
    context->lines->push_back(LevelPrefix(entry.level) + std::string(entry.message));
}

void AppendCommandOutputEntry(const Command::CommandOutputLineView& entry, void* user_data) {
    auto* context = static_cast<PumpCommandOutputContext*>(user_data);
    if (!context || !context->lines) {
        return;
    }
    context->lines->push_back(std::string(entry.text));
}

void AppendAutocompleteCandidate(const Command::CommandCandidateView& candidate, void* user_data) {
    auto* context = static_cast<CollectAutocompleteContext*>(user_data);
    if (!context || !context->candidates) {
        return;
    }

    context->candidates->push_back(ConsoleSystem::AutocompleteCandidate{
        .text = std::string(candidate.text),
        .helper = std::string(candidate.helper),
        .is_command = candidate.is_command,
    });
}

} // namespace

ConsoleSystem::ConsoleSystem(
    const SharedPtr<EditorConfig>& editor_config,
    Command::EngineCommandProcessor& command_processor
) :
    m_editor_config(editor_config),
    m_command_processor(command_processor) {
    if (m_editor_config) {
        Render::EditorConsoleVariables::ApplyToEditorConfig(*m_editor_config);
    }
}

bool ConsoleSystem::IsEditorConsoleOpen() const {
    return m_editor_console_page_open;
}

void ConsoleSystem::SetEditorConsoleOpen(bool open) {
    if (open == m_editor_console_page_open) {
        return;
    }
    m_editor_console_page_open = open;
    m_focus_window_next_frame  = open;
    m_focus_input_next_frame   = open;
    m_focus_input_frames       = open ? 3 : 0;
}

void ConsoleSystem::ExecuteCommand(std::string_view line) {
    const std::string trimmed = Trim(line);
    if (trimmed.empty()) {
        return;
    }

    m_command_history.push_back(trimmed);
    AppendOutput("> " + trimmed);
    m_history_cursor = -1;
    m_history_backup.clear();

    m_command_processor << trimmed;
}

void ConsoleSystem::TickUI() {
    m_editor_console_input_active   = false;

    if (m_editor_config) {
        Render::EditorConsoleVariables::CaptureFromEditorConfig(*m_editor_config);
    }

    PumpRuntimeLogs();
    PumpCommandOutputs();
    HandleHotkeys();

    if (IsRenderConsoleVisible()) {
        if (m_mode == EDisplayMode::Inline) {
            DrawRenderInlineConsole();
        } else {
            DrawRenderWindowedConsole();
        }
    }

    if (m_editor_console_page_open) {
        DrawEditorConsolePanel();
    }

    if (m_editor_config) {
        Render::EditorConsoleVariables::ApplyToEditorConfig(*m_editor_config);
    }

    const bool play_capture = m_editor_config && m_editor_config->play_mode_enabled &&
                              m_editor_config->play_mode_capture_input;
    const bool editor_console_interacting = IsEditorConsoleInteracting();
    const bool console_requests_cursor = editor_console_interacting || IsRenderConsoleVisible();
    WindowInput::Get().force_cursor_visible = console_requests_cursor;
    WindowInput::Get().block_camera_keyboard_input =
        (editor_console_interacting || IsRenderConsoleVisible()) &&
        (ImGui::GetIO().WantCaptureKeyboard || ImGui::GetIO().WantTextInput || m_focus_input_next_frame);

    if (!play_capture) {
        WindowInput::Get().force_cursor_visible = false;
        WindowInput::Get().block_camera_keyboard_input = editor_console_interacting;
    }
}

void ConsoleSystem::PumpRuntimeLogs() {
    std::vector<std::string> lines;
    PumpLogsContext context{.lines = &lines};
    if (!LogSystem::PollConsoleLogs(m_next_log_sequence, &AppendRuntimeLogEntry, &context, 256)) {
        return;
    }
    for (const std::string& line_text : lines) {
        AppendOutput(line_text);
    }
}

void ConsoleSystem::PumpCommandOutputs() {
    std::vector<std::string> lines;
    PumpCommandOutputContext context{.lines = &lines};
    if (!m_command_processor.PollOutput(
            m_next_command_output_sequence,
            &AppendCommandOutputEntry,
            &context,
            256
        )) {
        return;
    }
    for (const std::string& line_text : lines) {
        AppendOutput(line_text);
    }
}

void ConsoleSystem::HandleHotkeys() {
    if (ImGui::GetIO().WantTextInput) {
        m_last_grave_switch_state = WindowInput::Get().key_button_switch_state[KeyButtons::GRAVE_ACCENT];
        m_last_escape_down = WindowInput::Get().key_button_state[KeyButtons::ESCAPE];
        return;
    }

    const bool grave_switch = WindowInput::Get().key_button_switch_state[KeyButtons::GRAVE_ACCENT];
    if (grave_switch != m_last_grave_switch_state) {
        m_last_grave_switch_state = grave_switch;
        if (IsRenderWindowActive()) {
            CycleMode();
        } else {
            ToggleEditorConsolePage();
        }
    }

    const bool escape_down = WindowInput::Get().key_button_state[KeyButtons::ESCAPE];
    if (escape_down && !m_last_escape_down) {
        if (m_editor_console_page_open) {
            m_editor_console_page_open = false;
            m_focus_window_next_frame  = false;
        } else if (m_mode != EDisplayMode::Hidden) {
            m_mode = EDisplayMode::Hidden;
            m_focus_window_next_frame = false;
        }
    }
    m_last_escape_down = escape_down;
}

void ConsoleSystem::CycleMode() {
    switch (m_mode) {
        case EDisplayMode::Hidden:
            m_mode = EDisplayMode::Inline;
            break;
        case EDisplayMode::Inline:
            m_mode = EDisplayMode::Windowed;
            break;
        case EDisplayMode::Windowed:
            m_mode = EDisplayMode::Hidden;
            break;
    }
    m_focus_input_next_frame  = IsRenderConsoleVisible();
    m_focus_window_next_frame = IsRenderConsoleVisible();
    m_focus_input_frames      = IsRenderConsoleVisible() ? 3 : 0;
}

void ConsoleSystem::ToggleEditorConsolePage() {
    m_editor_console_page_open = !m_editor_console_page_open;
    m_focus_input_next_frame   = m_editor_console_page_open;
    m_focus_window_next_frame  = m_editor_console_page_open;
    m_focus_input_frames       = m_editor_console_page_open ? 3 : 0;
}

bool ConsoleSystem::IsRenderWindowActive() const {
    return WindowInput::Get().is_active;
}

bool ConsoleSystem::IsRenderConsoleVisible() const {
    return IsRenderWindowActive() && m_mode != EDisplayMode::Hidden;
}

void ConsoleSystem::AppendOutput(std::string line) {
    if (line.empty()) {
        return;
    }
    m_output_lines.push_back(std::move(line));
    while (m_output_lines.size() > k_max_output_lines) {
        m_output_lines.pop_front();
    }
}

void ConsoleSystem::DrawOutputPanel(const char* id, float input_height) {
    if (ImGui::BeginChild(id, ImVec2(0.0f, -input_height), true, ImGuiWindowFlags_HorizontalScrollbar)) {
        const bool should_scroll =
            m_auto_scroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2.0f;
        if (m_output_lines.empty()) {
            ImGui::TextUnformatted("No output yet.");
        } else {
            for (const std::string& line : m_output_lines) {
                ImGui::TextUnformatted(line.c_str());
            }
        }
        if (should_scroll) {
            ImGui::SetScrollHereY(1.0f);
        }
    }
    ImGui::EndChild();
}

void ConsoleSystem::DrawInputLine(const char* id) {
    std::string run_button_id = std::string("Run##") + id;

    UpdateAutocompleteCandidates();

    if (m_focus_input_next_frame || m_focus_input_frames > 0) {
        ImGui::SetKeyboardFocusHere();
        m_focus_input_next_frame = false;
        if (m_focus_input_frames > 0) {
            --m_focus_input_frames;
        }
    }

    InputCallbackUserData cb_user_data{.console = this};
    ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x - 78.0f, 120.0f));
    bool submit_by_enter = ImGui::InputTextWithHint(
        id,
        "Enter /command or cvar",
        m_input_buffer.data(),
        m_input_buffer.size(),
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory |
            ImGuiInputTextFlags_CallbackCompletion,
        [](ImGuiInputTextCallbackData* data) -> int {
            auto* user_data = static_cast<InputCallbackUserData*>(data->UserData);
            return user_data && user_data->console ? user_data->console->OnInputTextCallback(data) : 0;
        },
        &cb_user_data
    );
    m_editor_console_input_active =
        m_editor_console_input_active || ImGui::IsItemActive() || ImGui::IsItemFocused();
    ImGui::SameLine();
    bool submit_by_button = ImGui::Button(run_button_id.c_str(), ImVec2(70.0f, 0.0f));

    DrawAutocompleteList((std::string(id) + "##Autocomplete").c_str());

    if (submit_by_enter || submit_by_button) {
        ExecuteCommand(m_input_buffer.data());
        m_input_buffer.fill('\0');
        m_autocomplete_candidates.clear();
        m_autocomplete_selected = -1;
        m_last_autocomplete_query.clear();
        m_focus_input_next_frame = true;
    }
}

void ConsoleSystem::DrawEditorConsolePanel() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float width = std::min(viewport->WorkSize.x - 16.0f, 640.0f);
    const float height = 260.0f;
    const ImVec2 pos = ImVec2(
        viewport->WorkPos.x + 8.0f,
        viewport->WorkPos.y + viewport->WorkSize.y - height - 8.0f
    );

    ImGui::SetNextWindowPos(pos, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_FirstUseEver);
    if (m_focus_window_next_frame) {
        ImGui::SetNextWindowFocus();
    }

    bool open = m_editor_console_page_open;
    if (ImGui::Begin("Console", &open)) {
        if (m_focus_window_next_frame) {
            ImGui::SetWindowFocus();
            m_focus_window_next_frame = false;
        }

        ImGui::TextUnformatted("Usage: /command   or   <cvar> [value|?]");
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear")) {
            m_output_lines.clear();
            LogSystem::ClearConsoleLogs();
            m_command_processor.ClearOutput();
        }
        ImGui::SameLine();
        ImGui::Checkbox("AutoScroll", &m_auto_scroll);

        DrawOutputPanel("##EditorConsoleResidentOutput", 34.0f + GetAutocompletePanelHeight());
        DrawInputLine("##EditorConsoleResidentInput");
    }
    ImGui::End();

    if (!open) {
        m_editor_console_page_open = false;
        m_focus_window_next_frame = false;
    }
}

bool ConsoleSystem::IsEditorConsoleInteracting() const {
    return m_editor_console_page_open &&
           (m_editor_console_input_active || m_focus_input_next_frame || m_focus_input_frames > 0);
}

void ConsoleSystem::DrawRenderInlineConsole() {
    ImGuiWindow* scene_window = ImGui::FindWindowByName("Scene Color");
    if (!scene_window || !scene_window->WasActive) {
        return;
    }

    const ImRect bounds = scene_window->InnerRect;
    const float width = std::min(bounds.GetWidth() - 16.0f, 680.0f);
    const float height = std::min(bounds.GetHeight() * 0.45f, 220.0f);
    const ImVec2 pos = ImVec2(bounds.Min.x + 8.0f, bounds.Max.y - height - 8.0f);

    ImGui::SetNextWindowViewport(scene_window->ViewportId);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(std::max(width, 320.0f), std::max(height, 120.0f)), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.9f);
    if (m_focus_window_next_frame) {
        ImGui::SetNextWindowFocus();
    }

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoDocking;

    if (ImGui::Begin("##RenderConsoleInlineWindow", nullptr, flags)) {
        if (m_focus_window_next_frame) {
            ImGui::SetWindowFocus();
            m_focus_window_next_frame = false;
        }
        DrawOutputPanel("##RenderConsoleInlineOutput", 34.0f + GetAutocompletePanelHeight());
        DrawInputLine("##RenderConsoleInlineInput");
    }
    ImGui::End();
}

void ConsoleSystem::DrawRenderWindowedConsole() {
    ImGuiWindow* scene_window = ImGui::FindWindowByName("Scene Color");
    if (!scene_window || !scene_window->WasActive) {
        return;
    }

    const ImRect bounds = scene_window->InnerRect;
    ImGui::SetNextWindowViewport(scene_window->ViewportId);
    ImGui::SetNextWindowPos(ImVec2(bounds.Min.x + 24.0f, bounds.Min.y + 24.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(
        ImVec2(std::min(bounds.GetWidth() - 48.0f, 720.0f), std::min(bounds.GetHeight() - 48.0f, 420.0f)),
        ImGuiCond_Always
    );
    if (m_focus_window_next_frame) {
        ImGui::SetNextWindowFocus();
    }

    bool open = true;
    if (!ImGui::Begin("Render Console", &open, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        if (!open) {
            m_mode = EDisplayMode::Hidden;
            m_focus_window_next_frame = false;
        }
        return;
    }

    if (m_focus_window_next_frame) {
        ImGui::SetWindowFocus();
        m_focus_window_next_frame = false;
    }
    if (!open) {
        m_mode = EDisplayMode::Hidden;
        m_focus_window_next_frame = false;
    }

    ImGui::TextUnformatted("Render-scoped console sub display.");
    ImGui::SameLine();
    if (ImGui::Button("Hide")) {
        m_mode = EDisplayMode::Hidden;
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear##RenderConsole")) {
        m_output_lines.clear();
        LogSystem::ClearConsoleLogs();
        m_command_processor.ClearOutput();
    }
    ImGui::SameLine();
    ImGui::Checkbox("AutoScroll##RenderConsole", &m_auto_scroll);

    DrawOutputPanel("##RenderConsoleWindowedOutput", 34.0f + GetAutocompletePanelHeight());
    DrawInputLine("##RenderConsoleWindowedInput");
    ImGui::End();
}

void ConsoleSystem::UpdateAutocompleteCandidates() {
    UpdateAutocompleteCandidatesForInput(m_input_buffer.data());
}

void ConsoleSystem::UpdateAutocompleteCandidatesForInput(std::string_view raw_input) {
    const std::string query = Trim(raw_input);
    if (query.empty()) {
        m_autocomplete_candidates.clear();
        m_autocomplete_selected = -1;
        m_last_autocomplete_query.clear();
        return;
    }

    if (query != m_last_autocomplete_query) {
        m_autocomplete_selected = 0;
        m_last_autocomplete_query = query;
    }

    m_autocomplete_candidates.clear();

    CollectAutocompleteContext context{.candidates = &m_autocomplete_candidates};
    m_command_processor.VisitCandidates(query, &AppendAutocompleteCandidate, &context, 64);

    if (m_autocomplete_candidates.empty()) {
        m_autocomplete_selected = -1;
    } else if (m_autocomplete_selected < 0 ||
               m_autocomplete_selected >= static_cast<int>(m_autocomplete_candidates.size())) {
        m_autocomplete_selected = 0;
    }
}

void ConsoleSystem::DrawAutocompleteList(const char* id) {
    if (m_autocomplete_candidates.empty()) {
        return;
    }

    if (ImGui::BeginChild(id, ImVec2(0.0f, GetAutocompletePanelHeight()), true)) {
        for (int i = 0; i < static_cast<int>(m_autocomplete_candidates.size()); ++i) {
            const bool selected = (i == m_autocomplete_selected);
            std::string label = m_autocomplete_candidates[i].text;
            if (!m_autocomplete_candidates[i].helper.empty()) {
                label += "    ";
                label += m_autocomplete_candidates[i].helper;
            }
            if (ImGui::Selectable(label.c_str(), selected)) {
                m_autocomplete_selected = i;
                AcceptAutocompleteSelection();
            }
        }
    }
    ImGui::EndChild();
}

void ConsoleSystem::AcceptAutocompleteSelection() {
    const std::string text = GetSelectedAutocompleteText();
    if (text.empty()) {
        return;
    }

    SetInputBuffer(text);
    m_focus_input_next_frame = true;
    UpdateAutocompleteCandidates();
}

std::string ConsoleSystem::GetSelectedAutocompleteText() const {
    if (m_autocomplete_candidates.empty()) {
        return {};
    }
    int selected = m_autocomplete_selected;
    if (selected < 0 || selected >= static_cast<int>(m_autocomplete_candidates.size())) {
        selected = 0;
    }
    return m_autocomplete_candidates[selected].text + " ";
}

void ConsoleSystem::NavigateHistory(int delta) {
    if (m_command_history.empty()) {
        return;
    }

    if (m_history_cursor == -1) {
        m_history_backup = m_input_buffer.data();
    }

    if (delta < 0) {
        if (m_history_cursor < static_cast<int>(m_command_history.size()) - 1) {
            ++m_history_cursor;
        }
    } else if (delta > 0) {
        if (m_history_cursor >= 0) {
            --m_history_cursor;
        }
    }

    if (m_history_cursor >= 0) {
        const int index = static_cast<int>(m_command_history.size()) - 1 - m_history_cursor;
        SetInputBuffer(m_command_history[index]);
    } else {
        SetInputBuffer(m_history_backup);
    }

    m_focus_input_next_frame = true;
    UpdateAutocompleteCandidates();
}

void ConsoleSystem::SetInputBuffer(std::string_view text) {
    m_input_buffer.fill('\0');
    const size_t copy_len = std::min(text.size(), m_input_buffer.size() - 1);
    if (copy_len > 0) {
        std::memcpy(m_input_buffer.data(), text.data(), copy_len);
    }
}

float ConsoleSystem::GetAutocompletePanelHeight() const {
    return m_autocomplete_candidates.empty() ? 0.0f : 120.0f;
}

int ConsoleSystem::OnInputTextCallback(ImGuiInputTextCallbackData* data) {
    if (!data) {
        return 0;
    }

    UpdateAutocompleteCandidatesForInput(data->Buf ? data->Buf : "");

    if (data->EventFlag == ImGuiInputTextFlags_CallbackCompletion) {
        const std::string text = GetSelectedAutocompleteText();
        if (!text.empty()) {
            data->DeleteChars(0, data->BufTextLen);
            data->InsertChars(0, text.c_str());
            data->BufDirty = true;
            data->CursorPos = static_cast<int>(std::strlen(data->Buf));
            data->SelectionStart = data->SelectionEnd = data->CursorPos;
            SetInputBuffer(data->Buf);
            UpdateAutocompleteCandidates();
        }
        return 1;
    }

    if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
        if (!m_autocomplete_candidates.empty()) {
            if (data->EventKey == ImGuiKey_UpArrow) {
                if (m_autocomplete_selected <= 0) {
                    m_autocomplete_selected = static_cast<int>(m_autocomplete_candidates.size()) - 1;
                } else {
                    --m_autocomplete_selected;
                }
            } else if (data->EventKey == ImGuiKey_DownArrow) {
                if (m_autocomplete_selected < 0 ||
                    m_autocomplete_selected >= static_cast<int>(m_autocomplete_candidates.size()) - 1) {
                    m_autocomplete_selected = 0;
                } else {
                    ++m_autocomplete_selected;
                }
            }
            return 1;
        }

        if (data->EventKey == ImGuiKey_UpArrow) {
            NavigateHistory(-1);
        } else if (data->EventKey == ImGuiKey_DownArrow) {
            NavigateHistory(1);
        }
        data->DeleteChars(0, data->BufTextLen);
        data->InsertChars(0, m_input_buffer.data());
        data->BufDirty = true;
        data->CursorPos = static_cast<int>(std::strlen(data->Buf));
        data->SelectionStart = data->SelectionEnd = data->CursorPos;
        return 1;
    }

    return 0;
}

} // namespace Moer
