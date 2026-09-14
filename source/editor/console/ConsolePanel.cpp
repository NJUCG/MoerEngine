#include "console/ConsolePanel.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace Moer {

namespace {

struct CompletionTokenRange {
    std::size_t begin = 0;
    std::size_t end   = 0;
};

std::optional<CompletionTokenRange>
FindCompletionToken(std::string_view buffer, std::size_t cursor) {
    cursor = (std::min)(cursor, buffer.size());
    std::size_t token_begin = cursor;
    while (token_begin > 0 && buffer[token_begin - 1] != ' ' &&
           buffer[token_begin - 1] != '\t' && buffer[token_begin - 1] != '=') {
        --token_begin;
    }
    std::size_t first_token_begin = 0;
    while (first_token_begin < buffer.size() &&
           (buffer[first_token_begin] == ' ' || buffer[first_token_begin] == '\t')) {
        ++first_token_begin;
    }
    if (token_begin != first_token_begin) {
        return std::nullopt;
    }

    std::size_t token_end = cursor;
    while (token_end < buffer.size() && buffer[token_end] != ' ' &&
           buffer[token_end] != '\t' && buffer[token_end] != '=') {
        ++token_end;
    }
    return CompletionTokenRange{token_begin, token_end};
}

bool EqualsInsensitive(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto fold = [](char value) {
            return value >= 'A' && value <= 'Z' ?
                       static_cast<char>(value + ('a' - 'A')) :
                       value;
        };
        if (fold(left[index]) != fold(right[index])) {
            return false;
        }
    }
    return true;
}

} // namespace

ConsolePanel::ConsolePanel(std::shared_ptr<EngineCommandEndpoint> endpoint) : model(std::move(endpoint)) {}

void ConsolePanel::ShowWindow(bool* open) {
    input_active = false;
    static_cast<void>(model.Pump());
    const auto& output_lines = model.GetLines();
    const std::uint64_t newest_line_sequence =
        output_lines.empty() ? 0 : output_lines.back().session_sequence;
    if (newest_line_sequence != 0 &&
        newest_line_sequence != observed_line_sequence && auto_scroll) {
        scroll_to_bottom = true;
    }
    observed_line_sequence = newest_line_sequence;
    if (!ImGui::Begin("Console", open)) {
        ImGui::End();
        return;
    }

    if (ImGui::Button("Clear")) {
        model.Clear();
        observed_line_sequence = 0;
        scroll_to_bottom       = false;
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Auto-scroll", &auto_scroll) && !auto_scroll) {
        scroll_to_bottom = false;
    }
    ImGui::SameLine();
    filter.Draw("Filter", 240.0f);
    input_active = ImGui::IsItemActive() || ImGui::IsItemFocused();
    ImGui::Separator();

    const float footer_height = ImGui::GetFrameHeightWithSpacing();
    RefreshOutputMetrics(output_lines);
    if (!filter.IsActive() && output_content_width > 0.0f) {
        ImGui::SetNextWindowContentSize(ImVec2(output_content_width, 0.0f));
    }
    if (ImGui::BeginChild(
            "ConsoleOutput", ImVec2(0.0f, -footer_height), false, ImGuiWindowFlags_HorizontalScrollbar
        )) {
        const auto draw_row = [](const OutputVisualRow& row) {
            const ConsoleSessionLine& line = *row.line;
            ImGui::PushStyleColor(ImGuiCol_Text, LineColor(line.level));
            const char* text_begin = line.text.data() + row.text_begin;
            const char* text_end   = line.text.data() + row.text_end;
            if (row.show_source_label) {
                ImGui::TextUnformatted(SourceLabel(line.source));
                if (text_begin != text_end) {
                    ImGui::SameLine();
                    ImGui::TextUnformatted(text_begin, text_end);
                }
            } else {
                if (row.continuation_indent > 0.0f) {
                    ImGui::SetCursorPosX(
                        ImGui::GetCursorPosX() + row.continuation_indent
                    );
                }
                if (text_begin != text_end) {
                    ImGui::TextUnformatted(text_begin, text_end);
                } else {
                    ImGui::Dummy(ImVec2(0.0f, ImGui::GetTextLineHeight()));
                }
            }
            ImGui::PopStyleColor();
        };
        std::size_t displayed_row_count = output_visual_rows.size();
        if (filter.IsActive()) {
            displayed_row_count = 0;
            for (const OutputVisualRow& row : output_visual_rows) {
                if (filter.PassFilter(row.line->text.c_str())) {
                    draw_row(row);
                    ++displayed_row_count;
                }
            }
        } else {
            ImGuiListClipper clipper;
            clipper.Begin(
                static_cast<int>(output_visual_rows.size()),
                ImGui::GetTextLineHeightWithSpacing()
            );
            while (clipper.Step()) {
                for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
                    draw_row(output_visual_rows[static_cast<std::size_t>(index)]);
                }
            }
        }
        if (auto_scroll && scroll_to_bottom) {
            // ScrollMaxY still describes the previous frame while new rows are
            // being submitted. Request a value safely beyond this frame's
            // measured content; ImGui clamps it to the exact new maximum when
            // the child begins next frame. Keep it inside IM_TRUNC's int range.
            const double content_extent =
                static_cast<double>(displayed_row_count) *
                    ImGui::GetTextLineHeightWithSpacing() +
                ImGui::GetWindowHeight() + ImGui::GetStyle().WindowPadding.y * 2.0;
            const double max_safe_target =
                static_cast<double>((std::numeric_limits<int>::max)()) * 0.5;
            ImGui::SetScrollY(
                static_cast<float>((std::min)(content_extent, max_safe_target))
            );
            scroll_to_bottom = false;
        }
    }
    ImGui::EndChild();

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
    const bool input_focused = ImGui::IsItemActive() || ImGui::IsItemFocused();
    const ImVec2 input_min    = ImGui::GetItemRectMin();
    const ImVec2 input_max    = ImGui::GetItemRectMax();
    input_active = input_active || input_focused;
    ImGui::PopItemWidth();
    bool accepted_completion = false;
    if (submitted && completion_position >= 0 &&
        static_cast<std::size_t>(completion_position) <
            completion_candidates.size()) {
        const std::string_view input(input_buffer.data());
        const auto             token = FindCompletionToken(
            input,
            completion_token_end
        );
        if (token) {
            const std::string_view typed =
                input.substr(token->begin, token->end - token->begin);
            const std::string_view selected =
                completion_candidates[static_cast<std::size_t>(
                    completion_position
                )]
                    .text;
            if (!EqualsInsensitive(typed, selected)) {
                AcceptCompletion(
                    static_cast<std::size_t>(completion_position)
                );
                accepted_completion = true;
            }
        }
    }
    if (submitted && !accepted_completion) {
        const Command::ESubmitStatus status = model.Submit(input_buffer.data());
        if (status == Command::ESubmitStatus::Accepted || status == Command::ESubmitStatus::Empty) {
            input_buffer[0]  = '\0';
            history_position = -1;
            history_draft.clear();
            completion_candidates.clear();
            completion_position = -1;
        }
        reclaim_input_focus = true;
    }
    if (reclaim_input_focus) {
        ImGui::SetKeyboardFocusHere(-1);
        input_active        = true;
        reclaim_input_focus = false;
    }
    if (input_focused && !submitted) {
        DrawCompletionDropdown(input_min, input_max);
    }

    ImGui::End();
}

void ConsolePanel::RefreshOutputMetrics(
    const std::deque<ConsoleSessionLine>& lines
) {
    const ImFont* font         = ImGui::GetFont();
    const float   font_size    = ImGui::GetFontSize();
    const float   item_spacing = ImGui::GetStyle().ItemSpacing.x;
    const bool layout_changed =
        output_measurement_font != font ||
        output_measurement_font_size != font_size ||
        output_measurement_spacing != item_spacing;

    const auto reset_measurements = [&]() {
        output_line_measurements.clear();
        output_visual_rows.clear();
        output_line_widths.clear();
        output_content_width = 0.0f;
    };
    if (layout_changed) {
        reset_measurements();
        output_measurement_font      = font;
        output_measurement_font_size = font_size;
        output_measurement_spacing   = item_spacing;
    }
    if (lines.empty()) {
        reset_measurements();
        return;
    }

    const std::uint64_t first_sequence = lines.front().session_sequence;
    const std::uint64_t last_sequence  = lines.back().session_sequence;
    while (!output_line_measurements.empty() &&
           output_line_measurements.front().sequence < first_sequence) {
        const float removed_width = output_line_measurements.front().width;
        const auto  removed       = output_line_widths.find(removed_width);
        if (removed != output_line_widths.end()) {
            output_line_widths.erase(removed);
        }
        output_line_measurements.pop_front();
    }
    while (!output_visual_rows.empty() &&
           output_visual_rows.front().sequence < first_sequence) {
        output_visual_rows.pop_front();
    }
    if (!output_line_measurements.empty() &&
        (output_line_measurements.front().sequence != first_sequence ||
         output_line_measurements.back().sequence > last_sequence)) {
        reset_measurements();
    }

    const std::uint64_t measured_through =
        output_line_measurements.empty() ? 0 : output_line_measurements.back().sequence;
    for (const ConsoleSessionLine& line : lines) {
        if (line.session_sequence <= measured_through) {
            continue;
        }
        const bool  has_source_label = line.source_sequence != 0;
        const float leading_width =
            has_source_label ?
                ImGui::CalcTextSize(SourceLabel(line.source)).x + item_spacing :
                0.0f;
        float       line_width = 0.0f;
        std::size_t text_begin = 0;
        bool        first_row  = true;
        do {
            std::size_t text_end = line.text.find_first_of("\r\n", text_begin);
            if (text_end == std::string::npos) {
                text_end = line.text.size();
            }
            const char* row_begin = line.text.data() + text_begin;
            const char* row_end   = line.text.data() + text_end;
            const float row_width =
                leading_width + ImGui::CalcTextSize(row_begin, row_end).x;
            line_width = (std::max)(line_width, row_width);
            output_visual_rows.push_back({
                .sequence            = line.session_sequence,
                .line                = &line,
                .text_begin          = text_begin,
                .text_end            = text_end,
                .continuation_indent = !first_row && has_source_label ? leading_width : 0.0f,
                .show_source_label    = first_row && has_source_label,
            });
            first_row = false;
            if (text_end == line.text.size()) {
                break;
            }
            text_begin = text_end + 1;
            if (line.text[text_end] == '\r' && text_begin < line.text.size() &&
                line.text[text_begin] == '\n') {
                ++text_begin;
            }
        } while (text_begin < line.text.size());

        output_line_measurements.push_back({line.session_sequence, line_width});
        output_line_widths.insert(line_width);
    }
    output_content_width =
        output_line_widths.empty() ? 0.0f : *output_line_widths.rbegin();
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
        RefreshCompletionCandidates(
            data.Buf,
            static_cast<std::size_t>(data.BufTextLen),
            static_cast<std::size_t>(data.CursorPos)
        );
        history_position = -1;
        history_draft.clear();
    }
    return 0;
}

void ConsolePanel::RefreshCompletionCandidates(
    const char* text,
    std::size_t text_length,
    std::size_t cursor
) {
    std::string selected_text;
    if (completion_position >= 0 &&
        static_cast<std::size_t>(completion_position) <
            completion_candidates.size()) {
        selected_text =
            completion_candidates[static_cast<std::size_t>(
                completion_position
            )]
                .text;
    }
    const std::string_view buffer(text, text_length);
    const auto             token = FindCompletionToken(buffer, cursor);
    if (!token) {
        completion_candidates.clear();
        completion_position = -1;
        return;
    }

    completion_token_begin = token->begin;
    completion_token_end   = token->end;
    const std::string_view input_prefix =
        buffer.substr(token->begin, cursor - token->begin);
    completion_candidates = model.GetCandidates(input_prefix);
    completion_position = completion_candidates.empty() ? -1 : 0;
    if (!selected_text.empty()) {
        const auto selected = std::find_if(
            completion_candidates.begin(),
            completion_candidates.end(),
            [&](const Command::CommandCandidate& candidate) {
                return EqualsInsensitive(candidate.text, selected_text);
            }
        );
        if (selected != completion_candidates.end()) {
            completion_position = static_cast<int>(
                std::distance(completion_candidates.begin(), selected)
            );
        }
    }
}

void ConsolePanel::CompleteInput(ImGuiInputTextCallbackData& data) {
    RefreshCompletionCandidates(
        data.Buf,
        static_cast<std::size_t>(data.BufTextLen),
        static_cast<std::size_t>(data.CursorPos)
    );
    if (completion_position < 0 || completion_candidates.empty()) {
        return;
    }
    AcceptCompletion(data, static_cast<std::size_t>(completion_position));
}

void ConsolePanel::AcceptCompletion(
    ImGuiInputTextCallbackData& data,
    std::size_t                 candidate_index
) {
    if (candidate_index >= completion_candidates.size()) {
        return;
    }
    const std::string& replacement = completion_candidates[candidate_index].text;
    data.DeleteChars(
        static_cast<int>(completion_token_begin),
        static_cast<int>(completion_token_end - completion_token_begin)
    );
    data.InsertChars(
        static_cast<int>(completion_token_begin),
        replacement.data(),
        replacement.data() + replacement.size()
    );
    completion_candidates.clear();
    completion_position = -1;
}

void ConsolePanel::AcceptCompletion(std::size_t candidate_index) {
    if (candidate_index >= completion_candidates.size()) {
        return;
    }
    const std::size_t input_length = std::strlen(input_buffer.data());
    if (completion_token_begin > completion_token_end ||
        completion_token_end > input_length) {
        completion_candidates.clear();
        completion_position = -1;
        return;
    }

    std::string replacement;
    replacement.reserve(
        input_length - (completion_token_end - completion_token_begin) +
        completion_candidates[candidate_index].text.size()
    );
    replacement.append(input_buffer.data(), completion_token_begin);
    replacement += completion_candidates[candidate_index].text;
    replacement.append(
        input_buffer.data() + completion_token_end,
        input_length - completion_token_end
    );
    if (replacement.size() >= input_buffer.size()) {
        return;
    }
    std::memcpy(input_buffer.data(), replacement.data(), replacement.size());
    input_buffer[replacement.size()] = '\0';
    completion_candidates.clear();
    completion_position = -1;
    history_position    = -1;
    history_draft.clear();
    reclaim_input_focus = true;
}

void ConsolePanel::DrawCompletionDropdown(
    const ImVec2& input_min,
    const ImVec2& input_max
) {
    if (completion_candidates.empty()) {
        return;
    }

    constexpr std::size_t max_visible_candidates = 8;
    const std::size_t visible_count =
        (std::min)(completion_candidates.size(), max_visible_candidates);
    const float row_height = ImGui::GetFrameHeight();
    const float dropdown_height =
        row_height * static_cast<float>(visible_count) +
        ImGui::GetStyle().WindowPadding.y * 2.0f;
    const ImGuiViewport* viewport = ImGui::GetWindowViewport();
    float dropdown_y = input_max.y;
    if (viewport != nullptr &&
        dropdown_y + dropdown_height > viewport->WorkPos.y + viewport->WorkSize.y &&
        input_min.y - dropdown_height >= viewport->WorkPos.y) {
        dropdown_y = input_min.y - dropdown_height;
    }

    ImGui::SetNextWindowPos(ImVec2(input_min.x, dropdown_y));
    ImGui::SetNextWindowSize(ImVec2(input_max.x - input_min.x, dropdown_height));
    ImGui::SetNextWindowBgAlpha(0.98f);
    const ImGuiWindowFlags window_flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNavFocus;
    std::optional<std::size_t> accepted_candidate;
    if (ImGui::Begin("##ConsoleCompletionDropdown", nullptr, window_flags)) {
        for (std::size_t index = 0; index < completion_candidates.size(); ++index) {
            const Command::CommandCandidate& candidate = completion_candidates[index];
            std::string row = candidate.is_command ? "[command] " : "[cvar] ";
            row += candidate.text;
            if (!candidate.is_command) {
                row += " = ";
                row += candidate.value;
            }
            if (!candidate.helper.empty()) {
                row += "    ";
                row += candidate.helper;
            }
            ImGui::PushID(static_cast<int>(index));
            const bool selected = static_cast<int>(index) == completion_position;
            if (ImGui::Selectable(row.c_str(), selected)) {
                accepted_candidate = index;
            }
            if (selected) {
                ImGui::SetScrollHereY(0.5f);
            }
            if (ImGui::IsItemHovered() && !candidate.helper.empty()) {
                ImGui::SetTooltip("%s", candidate.helper.c_str());
            }
            ImGui::PopID();
        }
    }
    ImGui::End();
    if (accepted_candidate) {
        AcceptCompletion(*accepted_candidate);
    }
}

void ConsolePanel::NavigateHistory(ImGuiInputTextCallbackData& data, bool previous) {
    if (!completion_candidates.empty()) {
        if (previous) {
            completion_position = (std::max)(0, completion_position - 1);
        } else {
            completion_position = (std::min)(
                static_cast<int>(completion_candidates.size()) - 1,
                completion_position + 1
            );
        }
        return;
    }

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
    RefreshCompletionCandidates(
        data.Buf,
        static_cast<std::size_t>(data.BufTextLen),
        static_cast<std::size_t>(data.CursorPos)
    );
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
