#include "console/ConsoleSystem.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include <imgui.h>

#include "config/CVarSystem.h"
#include "log/LogSystem.h"
#include "window/WindowInput.h"

namespace Moer {
namespace {
struct InputCallbackUserData {
    ConsoleSystem* console = nullptr;
};

SharedPtr<EditorConfig> g_console_editor_config;

void ApplyParallelTranslateToConfig(const bool& old_value, const bool& new_value) {
    (void)old_value;
    if (g_console_editor_config) {
        g_console_editor_config->raytracing_config.process_light_cfg.parallel_mode = new_value;
    }
}

void ApplyNumThreadsToConfig(const int& old_value, const int& new_value) {
    (void)old_value;
    if (g_console_editor_config) {
        g_console_editor_config->raytracing_config.process_light_cfg.num_threads = std::max(1, new_value);
    }
}

CVar::TCVar<bool> s_cvar_rhi_parallel_translate_enable(
    TEXT("RHI.Translate.Parallel"),
    true,
    TEXT("Enable parallel translate mode for RHI command translation."),
    TEXT("true: use parallel translate."),
    TEXT("false: force single-thread translate."),
    ApplyParallelTranslateToConfig
);

CVar::TCVar<int> s_cvar_rhi_parallel_translate_num_threads(
    TEXT("RHI.Translate.NumThreads"),
    4,
    TEXT("Worker thread count used by RHI parallel translate."),
    TEXT(">=1 to increase parallel workers."),
    TEXT("invalid values are clamped to 1."),
    ApplyNumThreadsToConfig
);

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

} // namespace

ConsoleSystem::ConsoleSystem(const SharedPtr<EditorConfig>& editor_config) : m_editor_config(editor_config) {
    g_console_editor_config = editor_config;
    s_cvar_rhi_parallel_translate_enable.Set(
        editor_config->raytracing_config.process_light_cfg.parallel_mode
    );
    s_cvar_rhi_parallel_translate_num_threads.Set(
        std::max(1, editor_config->raytracing_config.process_light_cfg.num_threads)
    );
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
        const bool use_prefix = !prefix.empty();
        int        count      = 0;
        for (CVar::ICVar* cvar : CVar::GetAll()) {
            if (!cvar) {
                continue;
            }
            const std::string& name = cvar->GetName();
            if (!use_prefix || StartsWithInsensitive(name, prefix)) {
                AppendOutput(name + " (" + CVarTypeName(cvar->GetType()) + ") = " + cvar->GetValueAsString());
                ++count;
            }
        }
        if (count == 0) {
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
        AppendOutput(path + " = " + cvar->GetValueAsString());
        if (!cvar->GetHelper().empty()) {
            AppendOutput("  help: " + cvar->GetHelper());
        }
        if (cvar->GetType() == CVar::EType::Bool) {
            if (!cvar->GetTrueHelper().empty()) {
                AppendOutput("  true: " + cvar->GetTrueHelper());
            }
            if (!cvar->GetFalseHelper().empty()) {
                AppendOutput("  false: " + cvar->GetFalseHelper());
            }
        }
        return;
    }

    std::string error;
    if (!cvar->SetValueFromString(arg, error)) {
        AppendOutput("Error: " + error);
        return;
    }

    AppendOutput(path + " = " + cvar->GetValueAsString());
}

void ConsoleSystem::TickUI() {
    if (m_editor_config) {
        m_editor_config->raytracing_config.process_light_cfg.parallel_mode =
            s_cvar_rhi_parallel_translate_enable.Get();
        m_editor_config->raytracing_config.process_light_cfg.num_threads =
            std::max(1, s_cvar_rhi_parallel_translate_num_threads.Get());
    }

    PumpRuntimeLogs();
    HandleHotkeys();

    if (!IsVisible()) {
        WindowInput::Get().force_cursor_visible = false;
        WindowInput::Get().block_camera_keyboard_input = false;
        return;
    }
    const bool play_capture = m_editor_config && m_editor_config->play_mode_enabled &&
                              m_editor_config->play_mode_capture_input;
    WindowInput::Get().force_cursor_visible = !play_capture;

    if (m_mode == EDisplayMode::Inline) {
        DrawInlineConsole();
    } else {
        DrawWindowedConsole();
    }

    WindowInput::Get().block_camera_keyboard_input =
        IsVisible() && (ImGui::GetIO().WantCaptureKeyboard || m_focus_input_next_frame);
}

void ConsoleSystem::PumpRuntimeLogs() {
    std::vector<LogSystem::ConsoleLogEntry> logs;
    if (!LogSystem::PollConsoleLogs(m_next_log_sequence, logs, 256)) {
        return;
    }
    for (const auto& entry : logs) {
        AppendOutput(LevelPrefix(entry.level) + entry.message);
    }
}

void ConsoleSystem::HandleHotkeys() {
    const bool grave_switch = WindowInput::Get().key_button_switch_state[KeyButtons::GRAVE_ACCENT];
    if (grave_switch != m_last_grave_switch_state) {
        m_last_grave_switch_state = grave_switch;
        CycleMode();
    }

    const bool escape_down = WindowInput::Get().key_button_state[KeyButtons::ESCAPE];
    if (escape_down && !m_last_escape_down && IsVisible()) {
        m_mode = EDisplayMode::Hidden;
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
    m_focus_input_next_frame = IsVisible();
    m_focus_window_next_frame = IsVisible();
    m_focus_input_frames     = IsVisible() ? 3 : 0;
}

bool ConsoleSystem::IsVisible() const {
    return m_mode != EDisplayMode::Hidden;
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

    ImGui::PushItemWidth(-85.0f);
    if (m_focus_input_next_frame || m_focus_input_frames > 0) {
        ImGui::SetKeyboardFocusHere();
        m_focus_input_next_frame = false;
        if (m_focus_input_frames > 0) {
            --m_focus_input_frames;
        }
    }

    InputCallbackUserData cb_user_data{.console = this};
    bool submit_by_enter = ImGui::InputText(
        id,
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
    ImGui::PopItemWidth();
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

void ConsoleSystem::DrawInlineConsole() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float          width    = viewport->WorkSize.x - 16.0f;
    const float          height   = 180.0f;
    const ImVec2         pos      = ImVec2(viewport->WorkPos.x + 8.0f, viewport->WorkPos.y + viewport->WorkSize.y - height - 8.0f);

    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.9f);
    if (m_focus_window_next_frame) {
        ImGui::SetNextWindowFocus();
    }

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoDocking;

    if (ImGui::Begin("##ConsoleInlineWindow", nullptr, flags)) {
        if (m_focus_window_next_frame) {
            ImGui::SetWindowFocus();
            m_focus_window_next_frame = false;
        }
        DrawOutputPanel("##ConsoleInlineOutput", 34.0f + GetAutocompletePanelHeight());
        DrawInputLine("##ConsoleInlineInput");
    }
    ImGui::End();
}

void ConsoleSystem::DrawWindowedConsole() {
    bool open = true;
    if (m_focus_window_next_frame) {
        ImGui::SetNextWindowFocus();
    }
    if (!ImGui::Begin("Console", &open, ImGuiWindowFlags_NoDocking)) {
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

    ImGui::TextUnformatted("Usage: <path> ?   or   <path> <value>");
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        m_output_lines.clear();
        LogSystem::ClearConsoleLogs();
    }
    ImGui::SameLine();
    ImGui::Checkbox("AutoScroll", &m_auto_scroll);

    DrawOutputPanel("##ConsoleWindowedOutput", 34.0f + GetAutocompletePanelHeight());
    DrawInputLine("##ConsoleWindowedInput");

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
    for (CVar::ICVar* cvar : CVar::GetAll()) {
        if (!cvar) {
            continue;
        }
        const std::string& name = cvar->GetName();
        if (StartsWithInsensitive(name, token)) {
            m_autocomplete_candidates.push_back(name);
        }
    }

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
