#pragma once

#include "console/ConsoleSessionModel.h"

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <imgui.h>

namespace Moer {

class ConsolePanel {
public:
    explicit ConsolePanel(std::shared_ptr<EngineCommandEndpoint> endpoint);

    void ShowWindow(bool* open);

    [[nodiscard]] bool IsInputActive() const noexcept {
        return input_active;
    }

private:
    struct OutputLineMeasurement {
        std::uint64_t sequence = 0;
        float         width    = 0.0f;
    };

    struct OutputVisualRow {
        std::uint64_t             sequence = 0;
        const ConsoleSessionLine* line     = nullptr;
        std::size_t               text_begin = 0;
        std::size_t               text_end   = 0;
        float                     continuation_indent = 0.0f;
        bool                      show_source_label    = false;
    };

    static int InputCallback(ImGuiInputTextCallbackData* data);
    int        HandleInputCallback(ImGuiInputTextCallbackData& data);
    void       RefreshOutputMetrics(const std::deque<ConsoleSessionLine>& lines);
    void       RefreshCompletionCandidates(
        const char* text,
        std::size_t text_length,
        std::size_t cursor
    );
    void       CompleteInput(ImGuiInputTextCallbackData& data);
    void       AcceptCompletion(ImGuiInputTextCallbackData& data, std::size_t candidate_index);
    void       AcceptCompletion(std::size_t candidate_index);
    void       DrawCompletionDropdown(const ImVec2& input_min, const ImVec2& input_max);
    void       NavigateHistory(ImGuiInputTextCallbackData& data, bool previous);

    static ImVec4      LineColor(EConsoleSessionLevel level) noexcept;
    static const char* SourceLabel(EConsoleSessionSource source) noexcept;

    ConsoleSessionModel                    model;
    ImGuiTextFilter                        filter;
    std::array<char, 1024>                 input_buffer{};
    std::vector<Command::CommandCandidate> completion_candidates;
    int                                    completion_position = -1;
    std::size_t                            completion_token_begin = 0;
    std::size_t                            completion_token_end   = 0;
    int                                    history_position = -1;
    std::string                            history_draft;
    std::deque<OutputLineMeasurement>      output_line_measurements;
    std::deque<OutputVisualRow>            output_visual_rows;
    std::multiset<float>                   output_line_widths;
    const ImFont*                          output_measurement_font = nullptr;
    float                                  output_measurement_font_size = 0.0f;
    float                                  output_measurement_spacing  = 0.0f;
    float                                  output_content_width        = 0.0f;
    std::uint64_t                          observed_line_sequence = 0;
    bool                                   scroll_to_bottom       = false;
    bool                                   auto_scroll           = true;
    bool                                   reclaim_input_focus   = false;
    bool                                   input_active          = false;
};

} // namespace Moer
