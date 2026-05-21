#include "Editor.h"

#include "Engine.h"
#include "scene/Scene.h"
#include "scripting/ScriptExecutionRequest.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <string>

namespace {

using Clock = std::chrono::steady_clock;

constexpr auto k_before_script_delay = std::chrono::seconds(5);

enum class ETestPhase {
    WaitingSceneReady,
    WaitingBeforeScript,
    WaitingScriptResult,
    Finished,
};

struct TestState {
    ETestPhase                             phase = ETestPhase::WaitingSceneReady;
    Moer::scripting::ScriptExecutionFuture script_future;
    Clock::time_point                      phase_start_time      = {};
    int                                    last_countdown_second = -1;
    int                                    exit_code             = 0;
    bool                                   exit_requested        = false;
    bool                                   script_finished       = false;
};

std::filesystem::path ResolveExecutableDir(const char* argv0) {
    std::filesystem::path path = argv0;
    return path.filename().string().find(".exe") != std::string::npos ? path.parent_path() : path;
}

bool ReadTextFile(const std::filesystem::path& file_path, std::string& out_text) {
    std::ifstream stream(file_path, std::ios::in | std::ios::binary);
    if (!stream.is_open()) {
        return false;
    }

    out_text.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    return true;
}

void LogCountdown(
    const char*              prefix,
    const Clock::time_point& start_time,
    const Clock::duration&   total_duration,
    int&                     last_countdown_second
) {
    const auto elapsed = Clock::now() - start_time;
    const auto remaining =
        std::max(0.0, std::ceil(std::chrono::duration<double>(total_duration - elapsed).count()));
    const int seconds = static_cast<int>(remaining);
    if (seconds != last_countdown_second) {
        std::cout << prefix << seconds << "s" << std::endl;
        last_countdown_second = seconds;
    }
}

} // namespace

int main(int argc, const char** argv) {
    std::cout << "Moer Engine Editor-Based Mizuki Motion Manual Test Starting..." << std::endl;

    const std::filesystem::path executable_dir = ResolveExecutableDir(argv[0]);
    const std::filesystem::path script_path =
        executable_dir / "test" / "scripting" / "mizuki_motion_manual.py";

    Moer::Editor editor;
    TestState    state;

    editor.Init(argc, argv);
    editor.GetEngine().GetEditorConfig()->selected_render_method = Moer::ERenderMethod::Raster;

    editor.Run(Moer::Editor::ExtraHooks{.on_tick_test = [&](Moer::Scene& scene) {
        if (state.exit_requested || state.phase == ETestPhase::Finished) {
            return;
        }

        switch (state.phase) {
            case ETestPhase::WaitingSceneReady: {
                if (!scene.IsReady()) {
                    return;
                }

                state.phase                 = ETestPhase::WaitingBeforeScript;
                state.phase_start_time      = Clock::now();
                state.last_countdown_second = -1;
                std::cout << "[cpp] Scene ready. Waiting 5 seconds before Python import." << std::endl;
                return;
            }

            case ETestPhase::WaitingBeforeScript: {
                LogCountdown(
                    "[cpp] Script countdown: ",
                    state.phase_start_time,
                    k_before_script_delay,
                    state.last_countdown_second
                );

                if (Clock::now() - state.phase_start_time < k_before_script_delay) {
                    return;
                }

                std::string script_code;
                if (!ReadTextFile(script_path, script_code)) {
                    std::cerr << "[cpp] Failed to read script file: " << script_path << std::endl;
                    state.exit_code      = 3;
                    state.exit_requested = true;
                    editor.GetEngine().RequestExit();
                    return;
                }

                Moer::scripting::ScriptExecutionRequest request;
                request.source_name = script_path.filename().string();
                request.code        = std::move(script_code);
                state.script_future = editor.GetEngine().SubmitScriptExecution(std::move(request));
                state.phase         = ETestPhase::WaitingScriptResult;
                std::cout << "[cpp] Submitted script: " << script_path << std::endl;
                return;
            }

            case ETestPhase::WaitingScriptResult: {
                if (!state.script_future.valid() ||
                    state.script_future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                    return;
                }

                const Moer::scripting::ScriptExecutionResult result = state.script_future.get();
                std::cout << "[cpp] Script finished. success=" << (result.success ? "true" : "false")
                          << std::endl;
                if (!result.stdout_text.empty()) {
                    std::cout << "[cpp] Script stdout:\n" << result.stdout_text << std::endl;
                }
                if (!result.stderr_text.empty()) {
                    std::cout << "[cpp] Script stderr:\n" << result.stderr_text << std::endl;
                }
                if (!result.exception_text.empty()) {
                    std::cout << "[cpp] Script exception:\n" << result.exception_text << std::endl;
                }

                state.exit_code       = result.success ? 0 : 1;
                state.script_finished = true;
                state.phase           = ETestPhase::Finished;
                std::cout << "[cpp] Script finished. Editor will stay open for manual inspection."
                          << std::endl;
                return;
            }

            case ETestPhase::Finished:
                return;
        }
    }});

    if (!state.exit_requested && !state.script_finished) {
        state.exit_code = 0;
    }

    editor.ShutDown();
    return state.exit_code;
}