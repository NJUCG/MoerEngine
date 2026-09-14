#pragma once

#include "EngineConsoleControl.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Moer {

enum class EConsoleSessionSource : std::uint8_t {
    Session,
    Log,
    Command,
};

enum class EConsoleSessionLevel : std::uint8_t {
    Trace,
    Debug,
    Info,
    Warning,
    Error,
    Critical,
};

struct ConsoleSessionLine {
    std::uint64_t         session_sequence = 0;
    std::uint64_t         source_sequence  = 0;
    EConsoleSessionSource source           = EConsoleSessionSource::Session;
    EConsoleSessionLevel  level            = EConsoleSessionLevel::Info;
    std::string           text;
};

struct ConsoleSessionLimits {
    std::size_t display_capacity            = 2048;
    std::size_t history_capacity            = 128;
    std::size_t display_byte_capacity       = 8 * 1024 * 1024;
    std::size_t display_visual_row_capacity = 16 * 1024;
    std::size_t max_entry_bytes             = 64 * 1024;
    std::size_t max_visual_rows_per_entry   = 256;
    std::size_t max_visual_row_bytes        = 4096;
};

struct ConsoleSessionPumpResult {
    std::size_t   log_lines             = 0;
    std::size_t   command_lines         = 0;
    std::uint64_t dropped_log_lines     = 0;
    std::uint64_t dropped_command_lines = 0;
};

// Editor-owned, Game-Thread session state. The Core log and command channels
// retain independent sequence domains; session_sequence only records this
// model's deterministic display order (logs first, then command output per
// Pump call) and does not claim a cross-source global timeline.
class ConsoleSessionModel {
public:
    explicit ConsoleSessionModel(
        std::shared_ptr<EngineCommandEndpoint> endpoint,
        ConsoleSessionLimits                   limits = {}
    );

    [[nodiscard]] ConsoleSessionPumpResult
    Pump(std::size_t max_log_lines = 256, std::size_t max_command_lines = 256);
    [[nodiscard]] Command::ESubmitStatus Submit(std::string_view text);

    // Clear is local to this Editor session. It also advances both source
    // cursors to their current tails without deleting globally retained data.
    void Clear();

    [[nodiscard]] std::vector<Command::CommandCandidate>
    GetCandidates(std::string_view input, std::size_t max_count = 64) const;

    [[nodiscard]] const std::deque<ConsoleSessionLine>& GetLines() const noexcept {
        return lines;
    }
    [[nodiscard]] const std::deque<std::string>& GetHistory() const noexcept {
        return history;
    }

private:
    void Append(
        EConsoleSessionSource source,
        EConsoleSessionLevel  level,
        std::uint64_t         source_sequence,
        std::string           text
    );
    static std::string_view Trim(std::string_view text) noexcept;

    std::shared_ptr<EngineCommandEndpoint> endpoint;
    ConsoleSessionLimits                   limits;
    std::deque<ConsoleSessionLine>         lines;
    std::deque<std::string>                history;
    std::size_t                            displayed_text_bytes  = 0;
    std::size_t                            displayed_visual_rows = 0;
    std::uint64_t                          next_session_sequence = 1;
    std::uint64_t                          next_log_sequence     = 1;
    std::uint64_t                          next_command_sequence = 1;
};

} // namespace Moer
