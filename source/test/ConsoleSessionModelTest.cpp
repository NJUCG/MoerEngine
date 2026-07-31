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

} // namespace

int main() {
    LogSystem::Init();

    EngineConsoleControl control(EngineConsoleStartupConfig{}, 2);
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
