#include "console/ConsolePanel.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string_view>
#include <utility>

namespace Moer {

namespace {

char FoldAscii(char value) noexcept {
    if (value >= 'A' && value <= 'Z') {
        return static_cast<char>(value + ('a' - 'A'));
    }
    return value;
}

std::size_t CommonPrefixLength(const std::vector<Command::CommandCandidate>& candidates) {
    if (candidates.empty()) {
        return 0;
    }
    std::size_t prefix_length = candidates.front().text.size();
    for (std::size_t index = 1; index < candidates.size(); ++index) {
        prefix_length       = (std::min)(prefix_length, candidates[index].text.size());
        std::size_t matched = 0;
        while (matched < prefix_length &&
               FoldAscii(candidates.front().text[matched]) == FoldAscii(candidates[index].text[matched])) {
            ++matched;
        }
        prefix_length = matched;
    }
    return prefix_length;
}

} // namespace

ConsolePanel::ConsolePanel(std::shared_ptr<EngineCommandEndpoint> endpoint) : model(std::move(endpoint)) {}

void ConsolePanel::ShowWindow(bool* open) {
    input_active = false;
    static_cast<void>(model.Pump());
    if (!ImGui::Begin("Console", open)) {
        ImGui::End();
        return;
    }

    if (ImGui::Button("Clear")) {
        model.Clear();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &auto_scroll);
    ImGui::SameLine();
    filter.Draw("Filter", 240.0f);
    ImGui::Separator();

    const float footer_height =
        ImGui::GetFrameHeightWithSpacing() +
        (completion_candidates.empty() ? 0.0f : ImGui::GetTextLineHeightWithSpacing() * 3.0f);
    if (ImGui::BeginChild(
            "ConsoleOutput", ImVec2(0.0f, -footer_height), false, ImGuiWindowFlags_HorizontalScrollbar
        )) {
        const bool  was_at_bottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;
        const auto& lines         = model.GetLines();
        const auto  draw_line     = [](const ConsoleSessionLine& line) {
            ImGui::PushStyleColor(ImGuiCol_Text, LineColor(line.level));
            if (line.source_sequence != 0) {
                ImGui::TextUnformatted(SourceLabel(line.source));
                ImGui::SameLine();
            }
            ImGui::TextUnformatted(line.text.c_str());
            ImGui::PopStyleColor();
        };
        if (filter.IsActive()) {
            for (const ConsoleSessionLine& line : lines) {
                if (filter.PassFilter(line.text.c_str())) {
                    draw_line(line);
                }
            }
        } else {
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(lines.size()));
            while (clipper.Step()) {
                for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
                    draw_line(lines[static_cast<std::size_t>(index)]);
                }
            }
        }
        if (auto_scroll && was_at_bottom) {
            ImGui::SetScrollHereY(1.0f);
        }
    }
    ImGui::EndChild();

    if (!completion_candidates.empty()) {
        ImGui::TextDisabled("Candidates:");
        const std::size_t visible_count = (std::min)(completion_candidates.size(), std::size_t{8});
        for (std::size_t index = 0; index < visible_count; ++index) {
            if (index != 0) {
                ImGui::SameLine();
            }
            ImGui::TextUnformatted(completion_candidates[index].text.c_str());
        }
        if (completion_candidates.size() > visible_count) {
            ImGui::SameLine();
            ImGui::TextDisabled("(+%zu)", completion_candidates.size() - visible_count);
        }
    }

    const ImGuiInputTextFlags input_flags =
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCompletion |
        ImGuiInputTextFlags_CallbackHistory | ImGuiInputTextFlags_CallbackEdit;
    ImGui::PushItemWidth(-1.0f);
    const bool submitted = ImGui::InputText(
        "##ConsoleInput",
        input_buffer.data(),
        input_buffer.size(),
        input_flags,
        &ConsolePanel::InputCallback,
        this
    );
    input_active = ImGui::IsItemActive() || ImGui::IsItemFocused();
    ImGui::PopItemWidth();
    if (submitted) {
        const Command::ESubmitStatus status = model.Submit(input_buffer.data());
        if (status == Command::ESubmitStatus::Accepted || status == Command::ESubmitStatus::Empty) {
            input_buffer[0]  = '\0';
            history_position = -1;
            history_draft.clear();
            completion_candidates.clear();
        }
        reclaim_input_focus = true;
    }
    if (reclaim_input_focus) {
        ImGui::SetKeyboardFocusHere(-1);
        input_active        = true;
        reclaim_input_focus = false;
    }

    ImGui::End();
}

int ConsolePanel::InputCallback(ImGuiInputTextCallbackData* data) {
    return static_cast<ConsolePanel*>(data->UserData)->HandleInputCallback(*data);
}

int ConsolePanel::HandleInputCallback(ImGuiInputTextCallbackData& data) {
    if (data.EventFlag == ImGuiInputTextFlags_CallbackCompletion) {
        CompleteInput(data);
    } else if (data.EventFlag == ImGuiInputTextFlags_CallbackHistory) {
        NavigateHistory(data, data.EventKey == ImGuiKey_UpArrow);
    } else if (data.EventFlag == ImGuiInputTextFlags_CallbackEdit) {
        completion_candidates.clear();
        history_position = -1;
        history_draft.clear();
    }
    return 0;
}

void ConsolePanel::CompleteInput(ImGuiInputTextCallbackData& data) {
    const std::string_view buffer(data.Buf, static_cast<std::size_t>(data.BufTextLen));
    const std::size_t      cursor      = static_cast<std::size_t>(data.CursorPos);
    std::size_t            token_begin = cursor;
    while (token_begin > 0 && buffer[token_begin - 1] != ' ' && buffer[token_begin - 1] != '\t' &&
           buffer[token_begin - 1] != '=') {
        --token_begin;
    }
    std::size_t first_token_begin = 0;
    while (first_token_begin < buffer.size() &&
           (buffer[first_token_begin] == ' ' || buffer[first_token_begin] == '\t')) {
        ++first_token_begin;
    }
    if (token_begin != first_token_begin) {
        completion_candidates.clear();
        return;
    }

    std::size_t token_end = cursor;
    while (token_end < buffer.size() && buffer[token_end] != ' ' && buffer[token_end] != '\t' &&
           buffer[token_end] != '=') {
        ++token_end;
    }
    const std::string_view input_prefix = buffer.substr(token_begin, cursor - token_begin);
    completion_candidates               = model.GetCandidates(input_prefix);
    if (completion_candidates.empty()) {
        return;
    }

    const std::size_t prefix_length = CommonPrefixLength(completion_candidates);
    if (completion_candidates.size() == 1 || prefix_length > input_prefix.size()) {
        const std::string_view replacement =
            completion_candidates.size() == 1 ?
                std::string_view(completion_candidates.front().text) :
                std::string_view(completion_candidates.front().text).substr(0, prefix_length);
        data.DeleteChars(static_cast<int>(token_begin), static_cast<int>(token_end - token_begin));
        data.InsertChars(
            static_cast<int>(token_begin), replacement.data(), replacement.data() + replacement.size()
        );
        if (completion_candidates.size() == 1) {
            completion_candidates.clear();
        }
    }
}

void ConsolePanel::NavigateHistory(ImGuiInputTextCallbackData& data, bool previous) {
    const auto& history = model.GetHistory();
    if (history.empty()) {
        return;
    }

    if (previous) {
        if (history_position < 0) {
            history_draft.assign(data.Buf, static_cast<std::size_t>(data.BufTextLen));
            history_position = static_cast<int>(history.size()) - 1;
        } else if (history_position > 0) {
            --history_position;
        }
    } else if (history_position >= 0) {
        ++history_position;
        if (history_position >= static_cast<int>(history.size())) {
            history_position = -1;
        }
    } else {
        return;
    }

    const std::string_view replacement =
        history_position >= 0 ? std::string_view(history[static_cast<std::size_t>(history_position)]) :
                                std::string_view(history_draft);
    data.DeleteChars(0, data.BufTextLen);
    data.InsertChars(0, replacement.data(), replacement.data() + replacement.size());
    completion_candidates.clear();
}

ImVec4 ConsolePanel::LineColor(EConsoleSessionLevel level) noexcept {
    switch (level) {
        case EConsoleSessionLevel::Trace:
            return ImVec4(0.58f, 0.58f, 0.58f, 1.0f);
        case EConsoleSessionLevel::Debug:
            return ImVec4(0.65f, 0.72f, 0.82f, 1.0f);
        case EConsoleSessionLevel::Warning:
            return ImVec4(1.0f, 0.78f, 0.20f, 1.0f);
        case EConsoleSessionLevel::Error:
            return ImVec4(1.0f, 0.38f, 0.32f, 1.0f);
        case EConsoleSessionLevel::Critical:
            return ImVec4(1.0f, 0.18f, 0.18f, 1.0f);
        case EConsoleSessionLevel::Info:
            return ImGui::GetStyleColorVec4(ImGuiCol_Text);
    }
    return ImGui::GetStyleColorVec4(ImGuiCol_Text);
}

const char* ConsolePanel::SourceLabel(EConsoleSessionSource source) noexcept {
    switch (source) {
        case EConsoleSessionSource::Log:
            return "[Log]";
        case EConsoleSessionSource::Command:
            return "[Cmd]";
        case EConsoleSessionSource::Session:
            return "[Session]";
    }
    return "[Console]";
}

} // namespace Moer
