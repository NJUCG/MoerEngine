#pragma once

#include "API_Macro.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Moer::Command {

struct EngineCommandProcessorLimits {
    // Zero is normalized to one so every constructed processor remains usable.
    std::size_t pending_capacity = 256;
    std::size_t output_capacity  = 2048;
};

enum class ESubmitStatus : std::uint8_t {
    Accepted,
    Empty,
    QueueFull,
};

enum class EProcessStatus : std::uint8_t {
    Processed,
    Empty,
    Busy,
    MaxCommandsZero,
};

struct ProcessPendingResult {
    EProcessStatus status    = EProcessStatus::Empty;
    std::size_t    processed = 0;
};

struct CommandOutputLineView {
    std::uint64_t    sequence = 0;
    std::string_view text;
};

struct CommandOutputLine {
    std::uint64_t sequence = 0;
    std::string   text;
};

struct CommandOutputPollResult {
    std::uint64_t next_sequence = 1;
    std::uint64_t dropped_count = 0;
    std::size_t   visited_count = 0;
};

struct CommandOutputBatch {
    std::vector<CommandOutputLine> lines;
    std::uint64_t                  next_sequence = 1;
    std::uint64_t                  dropped_count = 0;
};

struct CommandCandidateView {
    std::string_view text;
    std::string_view helper;
    bool             is_command = false;
};

struct CommandCandidate {
    std::string text;
    std::string helper;
    bool        is_command = false;
};

// View strings are borrowed only for the visitor invocation. Callers that need
// to retain them must copy, as PollOutput/GetCandidates do.
using CommandOutputVisitor    = void (*)(const CommandOutputLineView& _line, void* _context);
using CommandCandidateVisitor = void (*)(const CommandCandidateView& _candidate, void* _context);

// SubmitText is multi-producer safe. ProcessPending is a serialized consumer:
// a concurrent or callback-reentrant drain returns Busy instead of blocking.
class EngineCommandProcessor {
public:
    CORE_API explicit EngineCommandProcessor(EngineCommandProcessorLimits _limits = {});
    CORE_API ~EngineCommandProcessor();

    EngineCommandProcessor(const EngineCommandProcessor&)            = delete;
    EngineCommandProcessor& operator=(const EngineCommandProcessor&) = delete;
    EngineCommandProcessor(EngineCommandProcessor&&)                 = delete;
    EngineCommandProcessor& operator=(EngineCommandProcessor&&)      = delete;

    [[nodiscard]] CORE_API ESubmitStatus        SubmitText(std::string_view _text);
    [[nodiscard]] CORE_API ProcessPendingResult ProcessPending(std::size_t _max_commands = 64);

    // Raw visitors are invoked after releasing processor locks. The owning
    // convenience wrappers below copy in the caller's allocation domain.
    [[nodiscard]] CORE_API CommandOutputPollResult VisitOutput(
        std::uint64_t        _next_sequence,
        std::size_t          _max_count,
        CommandOutputVisitor _visitor,
        void*                _context
    ) const;
    [[nodiscard]] CORE_API std::size_t VisitCandidates(
        std::string_view        _input,
        std::size_t             _max_count,
        CommandCandidateVisitor _visitor,
        void*                   _context
    ) const;

    [[nodiscard]] CommandOutputBatch
    PollOutput(std::uint64_t _next_sequence = 1, std::size_t _max_count = 256) const {
        CommandOutputBatch            batch;
        const CommandOutputPollResult result = VisitOutput(
            _next_sequence,
            _max_count,
            [](const CommandOutputLineView& _line, void* _context) {
                static_cast<CommandOutputBatch*>(_context)->lines.push_back({
                    .sequence = _line.sequence,
                    .text     = std::string(_line.text),
                });
            },
            &batch
        );
        batch.next_sequence = result.next_sequence;
        batch.dropped_count = result.dropped_count;
        return batch;
    }

    [[nodiscard]] std::vector<CommandCandidate>
    GetCandidates(std::string_view _input, std::size_t _max_count = 64) const {
        std::vector<CommandCandidate> candidates;
        static_cast<void>(VisitCandidates(
            _input,
            _max_count,
            [](const CommandCandidateView& _candidate, void* _context) {
                static_cast<std::vector<CommandCandidate>*>(_context)->push_back({
                    .text       = std::string(_candidate.text),
                    .helper     = std::string(_candidate.helper),
                    .is_command = _candidate.is_command,
                });
            },
            &candidates
        ));
        return candidates;
    }

    CORE_API void ClearOutput();

private:
    void ProcessCommand(std::string_view _text);
    void ProcessSlashCommand(std::string_view _text);
    void ProcessDefaultCVar(std::string_view _text);
    void AppendOutput(std::string_view _text);

    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Moer::Command
