#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "Core.h"

namespace Moer::Command {

struct CommandOutputLineView {
    uint64_t         sequence = 0;
    std::string_view text;
};

struct CommandCandidateView {
    std::string_view text;
    std::string_view helper;
    bool             is_command = false;
};

using CommandOutputVisitor = void (*)(const CommandOutputLineView& entry, void* user_data);
using CommandCandidateVisitor = void (*)(const CommandCandidateView& entry, void* user_data);

class CORE_API EngineCommandProcessor {
public:
    EngineCommandProcessor() = default;
    ~EngineCommandProcessor();
    EngineCommandProcessor(const EngineCommandProcessor&) = delete;
    EngineCommandProcessor& operator=(const EngineCommandProcessor&) = delete;

    void SubmitText(std::string_view text);
    EngineCommandProcessor& operator<<(std::string_view text);

    bool ProcessPending(size_t max_commands_per_frame = 64);
    bool PollOutput(
        uint64_t             &next_sequence,
        CommandOutputVisitor  visitor,
        void*                 user_data,
        size_t                max_count = 256
    );
    bool VisitCandidates(
        std::string_view        input,
        CommandCandidateVisitor visitor,
        void*                   user_data,
        size_t                  max_count = 64
    ) const;
    void ClearOutput();

private:
    void ProcessCommand(std::string_view text);
    void ProcessSlashCommand(std::string_view text);
    void ProcessDefaultCVar(std::string_view text);
    void AppendCommandHelp(std::string_view command_name, std::string_view helper, std::string_view usage);
    void AppendOutput(std::string_view text);

    struct PendingStorage;
    struct OutputStorage;

    PendingStorage* m_pending_storage = nullptr;
    OutputStorage*  m_output_storage  = nullptr;
};

} // namespace Moer::Command