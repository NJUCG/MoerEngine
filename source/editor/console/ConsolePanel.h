#pragma once

#include "console/ConsoleSessionModel.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

#include <imgui.h>

namespace Moer {

class ConsolePanel {
public:
    explicit ConsolePanel(std::shared_ptr<EngineCommandEndpoint> endpoint);

    void ShowWindow(bool* open);

private:
    static int InputCallback(ImGuiInputTextCallbackData* data);
    int        HandleInputCallback(ImGuiInputTextCallbackData& data);
    void       CompleteInput(ImGuiInputTextCallbackData& data);
    void       NavigateHistory(ImGuiInputTextCallbackData& data, bool previous);

    static ImVec4      LineColor(EConsoleSessionLevel level) noexcept;
    static const char* SourceLabel(EConsoleSessionSource source) noexcept;

    ConsoleSessionModel                    model;
    ImGuiTextFilter                        filter;
    std::array<char, 1024>                 input_buffer{};
    std::vector<Command::CommandCandidate> completion_candidates;
    int                                    history_position = -1;
    std::string                            history_draft;
    bool                                   auto_scroll         = true;
    bool                                   reclaim_input_focus = false;
};

} // namespace Moer
