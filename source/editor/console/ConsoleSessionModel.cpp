#include "console/ConsoleSessionModel.h"

#include "log/LogSystem.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string_view>
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

constexpr std::string_view TruncationMarker = "... [truncated]";

std::size_t Utf8PrefixLength(std::string_view text, std::size_t max_bytes) noexcept {
    if (text.size() <= max_bytes) {
        return text.size();
    }
    std::size_t prefix_length = max_bytes;
    while (prefix_length > 0 &&
           (static_cast<unsigned char>(text[prefix_length]) & 0xC0u) == 0x80u) {
        --prefix_length;
    }
    return prefix_length;
}

std::string TruncateUtf8(
    std::string_view text,
    std::size_t      max_bytes,
    bool             force_marker = false
) {
    if (!force_marker && text.size() <= max_bytes) {
        return std::string(text);
    }
    if (max_bytes <= TruncationMarker.size()) {
        return std::string(TruncationMarker.substr(0, max_bytes));
    }

    const std::size_t separator_bytes = text.empty() ? 0 : 1;
    const std::size_t prefix_budget =
        max_bytes - TruncationMarker.size() - separator_bytes;
    const std::size_t prefix_length = Utf8PrefixLength(text, prefix_budget);
    std::string       result(text.substr(0, prefix_length));
    if (!result.empty()) {
        result.push_back(' ');
    }
    result.append(TruncationMarker);
    return result;
}

std::string LimitDisplayText(
    std::string_view text,
    std::size_t      max_entry_bytes,
    std::size_t      max_visual_rows,
    std::size_t      max_visual_row_bytes
) {
    std::string result;
    result.reserve((std::min)(text.size(), max_entry_bytes));

    std::size_t text_begin = 0;
    std::size_t row_index  = 0;
    do {
        std::size_t text_end = text.find_first_of("\r\n", text_begin);
        if (text_end == std::string_view::npos) {
            text_end = text.size();
        }

        std::size_t next_begin = text_end;
        if (text_end != text.size()) {
            next_begin = text_end + 1;
            if (text[text_end] == '\r' && next_begin < text.size() &&
                text[next_begin] == '\n') {
                ++next_begin;
            }
        }
        const bool has_more_rows = next_begin < text.size();
        const bool row_limit_reached =
            row_index + 1 >= max_visual_rows && has_more_rows;
        const std::string_view row = text.substr(text_begin, text_end - text_begin);
        std::string limited_row = TruncateUtf8(
            row,
            max_visual_row_bytes,
            row_limit_reached || row.size() > max_visual_row_bytes
        );
        if (row_index != 0) {
            result.push_back('\n');
        }
        result.append(limited_row);
        ++row_index;

        if (row_limit_reached || text_end == text.size() || next_begin == text.size()) {
            break;
        }
        text_begin = next_begin;
    } while (row_index < max_visual_rows);

    return TruncateUtf8(result, max_entry_bytes, result.size() > max_entry_bytes);
}

std::size_t CountVisualRows(std::string_view text) noexcept {
    std::size_t row_count = 1;
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '\n') {
            ++row_count;
        }
    }
    return row_count;
}

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
        .display_byte_capacity =
            _limits.display_byte_capacity == 0 ? 1 : _limits.display_byte_capacity,
        .display_visual_row_capacity =
            _limits.display_visual_row_capacity == 0 ? 1 : _limits.display_visual_row_capacity,
        .max_entry_bytes = _limits.max_entry_bytes == 0 ? 1 : _limits.max_entry_bytes,
        .max_visual_rows_per_entry =
            _limits.max_visual_rows_per_entry == 0 ? 1 : _limits.max_visual_rows_per_entry,
        .max_visual_row_bytes =
            _limits.max_visual_row_bytes == 0 ? 1 : _limits.max_visual_row_bytes,
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
    displayed_text_bytes  = 0;
    displayed_visual_rows = 0;

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
    const std::size_t effective_entry_limit =
        (std::min)(limits.max_entry_bytes, limits.display_byte_capacity);
    const std::size_t effective_visual_row_limit =
        (std::min)(
            limits.max_visual_rows_per_entry,
            limits.display_visual_row_capacity
        );
    text = LimitDisplayText(
        text,
        effective_entry_limit,
        effective_visual_row_limit,
        limits.max_visual_row_bytes
    );
    const std::size_t visual_rows = CountVisualRows(text);
    displayed_text_bytes += text.size();
    displayed_visual_rows += visual_rows;
    lines.push_back({
        .session_sequence = next_session_sequence++,
        .source_sequence  = source_sequence,
        .source           = source,
        .level            = level,
        .text             = std::move(text),
    });
    while (lines.size() > limits.display_capacity ||
           displayed_text_bytes > limits.display_byte_capacity ||
           displayed_visual_rows > limits.display_visual_row_capacity) {
        displayed_text_bytes -= lines.front().text.size();
        displayed_visual_rows -= CountVisualRows(lines.front().text);
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
