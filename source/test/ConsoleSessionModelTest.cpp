#include "console/ConsoleSessionModel.h"

#include "config/CVarSystem.h"
#include "log/LogSystem.h"
#include "renderer/EditorConfig.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

using namespace Moer;

[[noreturn]] void Fail(const char* message) {
    std::cerr << "ConsoleSessionModelContract failed: " << message << '\n';
    std::exit(1);
}

void Expect(bool condition, const char* message) {
    if (!condition) {
        Fail(message);
    }
}

const ConsoleSessionLine* FindLine(const ConsoleSessionModel& model, std::string_view text) {
    for (const ConsoleSessionLine& line : model.GetLines()) {
        if (line.text.find(text) != std::string::npos) {
            return &line;
        }
    }
    return nullptr;
}

std::size_t VisualRowCount(std::string_view text) {
    std::size_t count = 1;
    for (char value : text) {
        if (value == '\n') {
            ++count;
        }
    }
    return count;
}

bool VisualRowsFit(std::string_view text, std::size_t max_bytes) {
    std::size_t row_begin = 0;
    while (row_begin <= text.size()) {
        const std::size_t row_end = text.find('\n', row_begin);
        const std::size_t end =
            row_end == std::string_view::npos ? text.size() : row_end;
        if (end - row_begin > max_bytes) {
            return false;
        }
        if (row_end == std::string_view::npos) {
            return true;
        }
        row_begin = row_end + 1;
    }
    return true;
}

} // namespace

int main() {
    LogSystem::Init();

    EngineConsoleControl control(EngineConsoleStartupConfig{});
    EditorConfig         editor_config{};
    control.BindEditorConfig(editor_config);
    auto endpoint = control.GetCommandEndpoint();

    ConsoleSessionModel model(endpoint, {.display_capacity = 16, .history_capacity = 3});
    model.Clear();

    LOG_INFO("ConsoleSessionModel unique log line");
    Expect(
        endpoint->SubmitText("/help") == Command::ESubmitStatus::Accepted &&
            control.TickGameThread(editor_config) == 1,
        "command output fixture failed"
    );
    const ConsoleSessionPumpResult initial_pump = model.Pump();
    const ConsoleSessionLine*      log_line     = FindLine(model, "ConsoleSessionModel unique log line");
    const ConsoleSessionLine*      command_line = FindLine(model, "Commands:");
    Expect(
        initial_pump.log_lines >= 1 && initial_pump.command_lines >= 1 && log_line != nullptr &&
            command_line != nullptr && log_line->source == EConsoleSessionSource::Log &&
            command_line->source == EConsoleSessionSource::Command && log_line->source_sequence != 0 &&
            command_line->source_sequence != 0 && log_line->session_sequence < command_line->session_sequence,
        "two-source pump lost identity or deterministic logs-first display order"
    );

    Expect(
        model.Submit("  First.Command  ") == Command::ESubmitStatus::Accepted &&
            model.Submit("Second.Command") == Command::ESubmitStatus::Accepted &&
            model.Submit("Second.Command") == Command::ESubmitStatus::Accepted &&
            model.Submit("Third.Command") == Command::ESubmitStatus::Accepted &&
            model.Submit("Fourth.Command") == Command::ESubmitStatus::Accepted,
        "session submissions were rejected"
    );
    const auto& history = model.GetHistory();
    Expect(
        history.size() == 3 && history.front() == "Second.Command" && history.back() == "Fourth.Command",
        "history did not trim, deduplicate adjacent entries, or enforce its bound"
    );
    Expect(
        model.GetLines().size() <= 16 && FindLine(model, "> Fourth.Command") != nullptr,
        "display ring did not retain the newest bounded session line"
    );

    CVar::RegistrationResult candidate = CVar::RegisterBool(
        CVar::CVarDescriptor{
            .name   = "Console.Model.Candidate",
            .helper = "session autocomplete fixture",
        },
        false
    );
    const auto candidates = model.GetCandidates("console.model");
    Expect(
        candidate.Succeeded() && candidates.size() == 1 &&
            candidates.front().text == "Console.Model.Candidate",
        "session autocomplete did not use the Core command endpoint"
    );

    while (control.TickGameThread(editor_config, 64) != 0) {
    }
    ConsoleSessionModel bounded_model(endpoint, {.display_capacity = 8, .history_capacity = 2});
    bounded_model.Clear();
    for (int index = 0; index < 255; ++index) {
        Expect(
            endpoint->SubmitText("/queue-fill") == Command::ESubmitStatus::Accepted,
            "queue saturation fixture filled early"
        );
    }
    Expect(
        bounded_model.Submit("/help") == Command::ESubmitStatus::Accepted &&
            bounded_model.Submit("/help") == Command::ESubmitStatus::QueueFull &&
            FindLine(bounded_model, "queue is full") != nullptr,
        "queue saturation was not surfaced as a local session diagnostic"
    );
    while (control.TickGameThread(editor_config, 64) != 0) {
    }
    bounded_model.Clear();
    Expect(bounded_model.GetLines().empty(), "local clear retained display lines");
    LOG_WARNING("ConsoleSessionModel post-clear line");
    static_cast<void>(bounded_model.Pump());
    Expect(
        FindLine(bounded_model, "ConsoleSessionModel post-clear line") != nullptr &&
            FindLine(bounded_model, "Commands:") == nullptr,
        "local clear replayed old global source data or skipped new data"
    );

    ConsoleSessionModel limited_model(
        endpoint,
        {
            .display_capacity            = 8,
            .history_capacity            = 2,
            .display_byte_capacity       = 96,
            .display_visual_row_capacity = 4,
            .max_entry_bytes             = 64,
            .max_visual_rows_per_entry   = 2,
            .max_visual_row_bytes        = 32,
        }
    );
    limited_model.Clear();
    std::string long_utf8_row;
    for (int index = 0; index < 24; ++index) {
        long_utf8_row += "\xE4\xB8\xAD";
    }
    LOG_INFO("{}", long_utf8_row + "\nsecond-row\nthird-row");
    static_cast<void>(limited_model.Pump());
    Expect(
        limited_model.GetLines().size() == 1 &&
            limited_model.GetLines().front().text.size() <= 64 &&
            VisualRowCount(limited_model.GetLines().front().text) <= 2 &&
            VisualRowsFit(limited_model.GetLines().front().text, 32) &&
            limited_model.GetLines().front().text.find("[truncated]") != std::string::npos,
        "display entry, visual-row, or row-byte limit was not enforced"
    );
    const std::size_t first_marker =
        limited_model.GetLines().front().text.find(" ... [truncated]");
    Expect(
        first_marker != std::string::npos && first_marker % 3 == 0,
        "display truncation split a UTF-8 code point"
    );

    ConsoleSessionModel byte_budget_model(
        endpoint,
        {
            .display_capacity            = 8,
            .history_capacity            = 2,
            .display_byte_capacity       = 48,
            .display_visual_row_capacity = 8,
            .max_entry_bytes             = 32,
            .max_visual_rows_per_entry   = 2,
            .max_visual_row_bytes        = 32,
        }
    );
    byte_budget_model.Clear();
    LOG_INFO("budget-old-1234567890");
    static_cast<void>(byte_budget_model.Pump());
    LOG_INFO("budget-middle-123456");
    static_cast<void>(byte_budget_model.Pump());
    LOG_INFO("budget-new-1234567890");
    static_cast<void>(byte_budget_model.Pump());
    std::size_t displayed_bytes = 0;
    for (const ConsoleSessionLine& line : byte_budget_model.GetLines()) {
        displayed_bytes += line.text.size();
    }
    Expect(
        displayed_bytes <= 48 && FindLine(byte_budget_model, "budget-old") == nullptr &&
            FindLine(byte_budget_model, "budget-new") != nullptr,
        "aggregate display byte budget did not evict the oldest entries"
    );

    ConsoleSessionModel visual_budget_model(
        endpoint,
        {
            .display_capacity            = 8,
            .history_capacity            = 2,
            .display_byte_capacity       = 1024,
            .display_visual_row_capacity = 3,
            .max_entry_bytes             = 128,
            .max_visual_rows_per_entry   = 4,
            .max_visual_row_bytes        = 64,
        }
    );
    visual_budget_model.Clear();
    LOG_INFO("visual-old-a\nvisual-old-b");
    static_cast<void>(visual_budget_model.Pump());
    LOG_INFO("visual-new-a\nvisual-new-b");
    static_cast<void>(visual_budget_model.Pump());
    Expect(
        FindLine(visual_budget_model, "visual-old") == nullptr &&
            FindLine(visual_budget_model, "visual-new") != nullptr,
        "aggregate visual-row budget did not evict the oldest entries"
    );

    for (int batch = 0; batch < 9; ++batch) {
        for (int index = 0; index < 256; ++index) {
            Expect(
                endpoint->SubmitText("/overwrite-fixture") == Command::ESubmitStatus::Accepted,
                "command overwrite fixture overflowed before its bounded drain"
            );
        }
        Expect(
            control.TickGameThread(editor_config, 256) == 256,
            "command overwrite fixture did not drain its batch"
        );
    }
    ConsoleSessionModel overwritten_model(endpoint, {.display_capacity = 4096, .history_capacity = 2});
    const ConsoleSessionPumpResult overwritten_pump = overwritten_model.Pump(0, 4096);
    Expect(
        overwritten_pump.dropped_command_lines > 0 &&
            FindLine(overwritten_model, "command output line(s) were overwritten") != nullptr,
        "source overwrite was not reported through a loss marker"
    );

    std::cout << "ConsoleSessionModelContract passed\n";
    return 0;
}
