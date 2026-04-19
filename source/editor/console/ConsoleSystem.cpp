#include "console/ConsoleSystem.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include <imgui.h>
#include <imgui_internal.h>

#include "config/CVarSystem.h"
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

struct CollectCVarListContext {
    std::vector<std::string>* lines      = nullptr;
    std::string_view prefix;
    bool              use_prefix = false;
    int               count      = 0;
};

struct CollectAutocompleteContext {
    std::vector<std::string>* candidates = nullptr;
    std::string_view          token;
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

std::string ToLower(std::string_view in) {
    std::string out(in);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
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

bool StartsWithInsensitive(std::string_view text, std::string_view prefix) {
    if (prefix.size() > text.size()) {
        return false;
    }
    for (size_t i = 0; i < prefix.size(); ++i) {
        const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(text[i])));
        const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(prefix[i])));
        if (a != b) {
            return false;
        }
    }
    return true;
}

std::string CVarTypeName(CVar::EType type) {
    switch (type) {
        case CVar::EType::Bool:
            return "bool";
        case CVar::EType::Int:
            return "int";
        case CVar::EType::Float:
            return "float";
        case CVar::EType::String:
            return "string";
        default:
            return "unknown";
    }
}

std::string GetCVarValueText(const CVar::ICVar* cvar) {
    if (!cvar) {
        return {};
    }

    char buffer[512]{};
    cvar->CopyValueString(buffer, sizeof(buffer));
    return buffer;
}

void AppendRuntimeLogEntry(const LogSystem::ConsoleLogEntryView& entry, void* user_data) {
    auto* context = static_cast<PumpLogsContext*>(user_data);
    if (!context || !context->lines) {
        return;
    }
    context->lines->push_back(LevelPrefix(entry.level) + std::string(entry.message));
}

void AppendCVarListEntry(CVar::ICVar* cvar, void* user_data) {
    auto* context = static_cast<CollectCVarListContext*>(user_data);
    if (!context || !context->lines || !cvar) {
        return;
    }

    const std::string name(cvar->GetName());
    if (context->use_prefix && !StartsWithInsensitive(name, context->prefix)) {
        return;
    }

    context->lines->push_back(
        name + " (" + CVarTypeName(cvar->GetType()) + ") = " + GetCVarValueText(cvar)
    );
    ++context->count;
}

void AppendAutocompleteCandidate(CVar::ICVar* cvar, void* user_data) {
    auto* context = static_cast<CollectAutocompleteContext*>(user_data);
    if (!context || !context->candidates || !cvar) {
        return;
    }

    const std::string name(cvar->GetName());
    if (StartsWithInsensitive(name, context->token)) {
        context->candidates->push_back(name);
    }
}

} // namespace

ConsoleSystem::ConsoleSystem(const SharedPtr<EditorConfig>& editor_config) : m_editor_config(editor_config) {
    if (m_editor_config) {
        Render::EditorConsoleVariables::ApplyToEditorConfig(*m_editor_config);
    }
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

    if (ToLower(trimmed) == "help") {
        AppendOutput("Commands:");
        AppendOutput("  help");
        AppendOutput("  cvar list [prefix]");
        AppendOutput("  <cvar.path> ?");
        AppendOutput("  <cvar.path> <value>");
        return;
    }

    if (StartsWithInsensitive(trimmed, "cvar list")) {
        std::string prefix;
        if (trimmed.size() > 9) {
            prefix = Trim(trimmed.substr(9));
        }
        std::vector<std::string> lines;
        CollectCVarListContext context{
            .lines      = &lines,
            .prefix     = prefix,
            .use_prefix = !prefix.empty(),
            .count      = 0,
        };
        CVar::VisitAll(&AppendCVarListEntry, &context);
        for (const std::string& line_text : lines) {
            AppendOutput(line_text);
        }
        if (context.count == 0) {
            AppendOutput("No cvar matches prefix: " + prefix);
        }
        return;
    }

    const size_t first_space = trimmed.find_first_of(" \t");
    if (first_space == std::string::npos) {
        AppendOutput("Error: Missing argument. Usage: <path> <value|?>, help, or cvar list [prefix]");
        return;
    }

    const std::string path = trimmed.substr(0, first_space);
    std::string       arg  = Trim(trimmed.substr(first_space + 1));
    if (arg.empty()) {
        AppendOutput("Error: Missing argument. Usage: <path> <value|?>");
        return;
    }

    CVar::ICVar* cvar = CVar::Find(path);
    if (!cvar) {
        AppendOutput("Error: Unknown cvar: " + path);
        return;
    }

    if (arg == "?") {
        AppendOutput(path + " = " + GetCVarValueText(cvar));
        if (!cvar->GetHelper().empty()) {
            AppendOutput("  help: " + std::string(cvar->GetHelper()));
        }
        if (cvar->GetType() == CVar::EType::Bool) {
            if (!cvar->GetTrueHelper().empty()) {
                AppendOutput("  true: " + std::string(cvar->GetTrueHelper()));
            }
            if (!cvar->GetFalseHelper().empty()) {
                AppendOutput("  false: " + std::string(cvar->GetFalseHelper()));
            }
        }
        return;
    }

    if (const char* error = cvar->SetValueFromString(arg)) {
        AppendOutput(std::string("Error: ") + error);
        return;
    }

    AppendOutput(path + " = " + GetCVarValueText(cvar));
}

void ConsoleSystem::TickUI() {
    m_editor_console_input_active   = false;

    if (m_editor_config) {
        Render::EditorConsoleVariables::CaptureFromEditorConfig(*m_editor_config);
    }

    PumpRuntimeLogs();
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

void ConsoleSystem::HandleHotkeys() {
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
        "Enter console command",
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
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows) &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            m_focus_input_next_frame = true;
            m_focus_input_frames = std::max(m_focus_input_frames, 2);
        }

        ImGui::TextUnformatted("Usage: <path> ?   or   <path> <value>");
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear")) {
            m_output_lines.clear();
            LogSystem::ClearConsoleLogs();
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
    const size_t      space_pos = query.find_first_of(" \t");
    const std::string token = (space_pos == std::string::npos) ? query : query.substr(0, space_pos);

    if (token.empty()) {
        m_autocomplete_candidates.clear();
        m_autocomplete_selected = -1;
        m_last_autocomplete_query.clear();
        return;
    }

    if (token != m_last_autocomplete_query) {
        m_autocomplete_selected = 0;
        m_last_autocomplete_query = token;
    }

    m_autocomplete_candidates.clear();

    if (StartsWithInsensitive("help", token)) {
        m_autocomplete_candidates.push_back("help");
    }
    if (StartsWithInsensitive("cvar", token)) {
        m_autocomplete_candidates.push_back("cvar list");
    }
    CollectAutocompleteContext context{
        .candidates = &m_autocomplete_candidates,
        .token      = token,
    };
    CVar::VisitAll(&AppendAutocompleteCandidate, &context);

    std::sort(m_autocomplete_candidates.begin(), m_autocomplete_candidates.end());
    m_autocomplete_candidates.erase(
        std::unique(m_autocomplete_candidates.begin(), m_autocomplete_candidates.end()),
        m_autocomplete_candidates.end()
    );

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
            if (ImGui::Selectable(m_autocomplete_candidates[i].c_str(), selected)) {
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
    std::string text = m_autocomplete_candidates[selected];
    if (CVar::Find(text) || text == "cvar list") {
        text += " ";
    }
    return text;
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
