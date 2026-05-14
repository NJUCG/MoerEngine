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

constexpr auto k_before_script_delay = std::chrono::seconds(1);

enum class ETestPhase {
    WaitingSceneReady,
    WaitingBeforeScript,
    WaitingScriptResult,
    Finished,
};

struct TestState {
    ETestPhase                                          phase = ETestPhase::WaitingSceneReady;
    std::future<Moer::scripting::ScriptExecutionResult> script_future;
    Clock::time_point                                   phase_start_time      = {};
    int                                                 last_countdown_second = -1;
    int                                                 exit_code             = 3;
    bool                                                exit_requested        = false;
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

bool ValidateSceneStillUsable(Moer::Scene& scene) {
    if (!scene.IsReady()) {
        std::cout << "[cpp] Validation failed: scene is not ready after script execution." << std::endl;
        return false;
    }

    const entt::entity root = scene.GetRootNodeEntity();
    if (!scene.IsValidNodeEntity(root)) {
        std::cout << "[cpp] Validation failed: root node is invalid after script execution." << std::endl;
        return false;
    }

    std::cout << "[cpp] Post-script scene validation passed." << std::endl;
    return true;
}

} // namespace

int main(int argc, const char** argv) {
    std::cout << "Moer Engine Scripting 260514 All Test Starting..." << std::endl;

    const std::filesystem::path executable_dir = ResolveExecutableDir(argv[0]);
    const std::filesystem::path script_path =
        executable_dir / "test" / "scripting" / "scene_api_all_260514.py";

    Moer::Engine engine;
    TestState    state;

    engine.Init(argc, argv);
    engine.GetEditorConfig()->selected_render_method = Moer::ERenderMethod::Raster;

    engine.Run(Moer::Render::EngineHooks{.on_tick_test = [&](Moer::Scene& scene) {
        if (state.exit_requested) {
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
                std::cout << "[cpp] Scene ready. Waiting 1 second before script execution." << std::endl;
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
                    engine.RequestExit();
                    return;
                }

                Moer::scripting::ScriptExecutionRequest request;
                request.source_name = script_path.filename().string();
                request.code        = std::move(script_code);
                state.script_future = engine.SubmitScriptSnippet(std::move(request));
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

                if (!result.success) {
                    state.exit_code = 1;
                } else if (result.stdout_text.find("[py] scene api all 260514 done") == std::string::npos) {
                    std::cout << "[cpp] Validation failed: completion marker missing from script stdout."
                              << std::endl;
                    state.exit_code = 2;
                } else {
                    state.exit_code = ValidateSceneStillUsable(scene) ? 0 : 2;
                }

                state.phase          = ETestPhase::Finished;
                state.exit_requested = true;
                std::cout << "[cpp] Requesting engine shutdown." << std::endl;
                engine.RequestExit();
                return;
            }

            case ETestPhase::Finished:
                return;
        }
    }});

    if (!state.exit_requested) {
        state.exit_code = 0;
    }

    engine.ShutDown();
    return state.exit_code;
}
