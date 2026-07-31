#include "console/ConsoleSessionModel.h"

#include "log/LogSystem.h"

#include <limits>
#include <stdexcept>
#include <utility>

namespace Moer {

namespace {

EConsoleSessionLevel ToSessionLevel(spdlog::level::level_enum level) noexcept {
    switch (level) {
        case spdlog::level::trace:
            return EConsoleSessionLevel::Trace;
        case spdlog::level::debug:
            return EConsoleSessionLevel::Debug;
        case spdlog::level::warn:
            return EConsoleSessionLevel::Warning;
        case spdlog::level::err:
            return EConsoleSessionLevel::Error;
        case spdlog::level::critical:
            return EConsoleSessionLevel::Critical;
        case spdlog::level::info:
        case spdlog::level::off:
        case spdlog::level::n_levels:
            return EConsoleSessionLevel::Info;
    }
    return EConsoleSessionLevel::Info;
}

void DiscardLogLine(const LogSystem::ConsoleLogEntryView&, void*) {}

std::shared_ptr<EngineCommandEndpoint> RequireEndpoint(std::shared_ptr<EngineCommandEndpoint> endpoint) {
    if (!endpoint) {
        throw std::invalid_argument("ConsoleSessionModel requires a valid EngineCommandEndpoint.");
    }
    return endpoint;
}

} // namespace

ConsoleSessionModel::ConsoleSessionModel(
    std::shared_ptr<EngineCommandEndpoint> _endpoint,
    ConsoleSessionLimits                   _limits
) :
    endpoint(RequireEndpoint(std::move(_endpoint))),
    limits({
        .display_capacity = _limits.display_capacity == 0 ? 1 : _limits.display_capacity,
        .history_capacity = _limits.history_capacity == 0 ? 1 : _limits.history_capacity,
    }) {}

ConsoleSessionPumpResult ConsoleSessionModel::Pump(std::size_t max_log_lines, std::size_t max_command_lines) {
    ConsoleSessionPumpResult pump_result;

    struct LogVisitorContext {
        struct Entry {
            std::uint64_t        sequence = 0;
            EConsoleSessionLevel level    = EConsoleSessionLevel::Info;
            std::string          text;
        };
        std::vector<Entry> entries;
    } log_context;
    const LogSystem::ConsoleLogPollResult log_result = LogSystem::VisitConsoleLogs(
        next_log_sequence,
        max_log_lines,
        [](const LogSystem::ConsoleLogEntryView& entry, void* context) {
            auto& visitor_context = *static_cast<LogVisitorContext*>(context);
            visitor_context.entries.push_back({
                .sequence = entry.sequence,
                .level    = ToSessionLevel(entry.level),
                .text     = std::string(entry.message),
            });
        },
        &log_context
    );
    next_log_sequence             = log_result.next_sequence;
    pump_result.log_lines         = log_context.entries.size();
    pump_result.dropped_log_lines = log_result.dropped_count;
    if (log_result.dropped_count != 0) {
        Append(
            EConsoleSessionSource::Session,
            EConsoleSessionLevel::Warning,
            0,
            "[console] " + std::to_string(log_result.dropped_count) +
                " log line(s) were overwritten before this session could read them."
        );
    }
    for (auto& entry : log_context.entries) {
        Append(EConsoleSessionSource::Log, entry.level, entry.sequence, std::move(entry.text));
    }

    Command::CommandOutputBatch command_batch =
        endpoint->PollOutput(next_command_sequence, max_command_lines);
    next_command_sequence             = command_batch.next_sequence;
    pump_result.command_lines         = command_batch.lines.size();
    pump_result.dropped_command_lines = command_batch.dropped_count;
    if (command_batch.dropped_count != 0) {
        Append(
            EConsoleSessionSource::Session,
            EConsoleSessionLevel::Warning,
            0,
            "[console] " + std::to_string(command_batch.dropped_count) +
                " command output line(s) were overwritten before this session could read them."
        );
    }
    for (auto& entry : command_batch.lines) {
        Append(
            EConsoleSessionSource::Command, EConsoleSessionLevel::Info, entry.sequence, std::move(entry.text)
        );
    }

    return pump_result;
}

Command::ESubmitStatus ConsoleSessionModel::Submit(std::string_view text) {
    text = Trim(text);
    if (text.empty()) {
        return Command::ESubmitStatus::Empty;
    }

    const Command::ESubmitStatus status = endpoint->SubmitText(text);
    if (status == Command::ESubmitStatus::Accepted) {
        const std::string command(text);
        if (history.empty() || history.back() != command) {
            history.push_back(command);
            while (history.size() > limits.history_capacity) {
                history.pop_front();
            }
        }
        Append(EConsoleSessionSource::Session, EConsoleSessionLevel::Info, 0, "> " + command);
    } else if (status == Command::ESubmitStatus::QueueFull) {
        Append(
            EConsoleSessionSource::Session,
            EConsoleSessionLevel::Error,
            0,
            "[console] Command queue is full; the command was not submitted."
        );
    } else if (status == Command::ESubmitStatus::Closed) {
        Append(
            EConsoleSessionSource::Session,
            EConsoleSessionLevel::Error,
            0,
            "[console] Engine command admission is closed."
        );
    }
    return status;
}

void ConsoleSessionModel::Clear() {
    lines.clear();

    const LogSystem::ConsoleLogPollResult log_result = LogSystem::VisitConsoleLogs(
        next_log_sequence, (std::numeric_limits<std::size_t>::max)(), &DiscardLogLine, nullptr
    );
    next_log_sequence = log_result.next_sequence;

    const Command::CommandOutputBatch command_batch =
        endpoint->PollOutput(next_command_sequence, (std::numeric_limits<std::size_t>::max)());
    next_command_sequence = command_batch.next_sequence;
}

std::vector<Command::CommandCandidate>
ConsoleSessionModel::GetCandidates(std::string_view input, std::size_t max_count) const {
    return endpoint->GetCandidates(input, max_count);
}

void ConsoleSessionModel::Append(
    EConsoleSessionSource source,
    EConsoleSessionLevel  level,
    std::uint64_t         source_sequence,
    std::string           text
) {
    lines.push_back({
        .session_sequence = next_session_sequence++,
        .source_sequence  = source_sequence,
        .source           = source,
        .level            = level,
        .text             = std::move(text),
    });
    while (lines.size() > limits.display_capacity) {
        lines.pop_front();
    }
}

std::string_view ConsoleSessionModel::Trim(std::string_view text) noexcept {
    while (!text.empty() &&
           (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' || text.front() == '\n')) {
        text.remove_prefix(1);
    }
    while (!text.empty() &&
           (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n')) {
        text.remove_suffix(1);
    }
    return text;
}

} // namespace Moer
