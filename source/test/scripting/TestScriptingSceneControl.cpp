#include "Engine.h"

#include "math/Quaternion.h"
#include "scene/Scene.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <string>
#include <string_view>

namespace {

using Clock = std::chrono::steady_clock;

constexpr auto             k_before_script_delay = std::chrono::seconds(5);
constexpr auto             k_after_script_delay  = std::chrono::seconds(5);
constexpr float            k_float_epsilon       = 1e-4f;
constexpr std::string_view k_expected_name       = "ScriptedRoot";
const Moer::float3         k_expected_translation(1.0f, 2.0f, 3.0f);
const Moer::Quaternion     k_expected_rotation(1.0f, 0.0f, 0.0f, 0.0f);
const Moer::float3         k_expected_scale(1.25f, 1.5f, 1.75f);

enum class ETestPhase {
    WaitingSceneReady,
    WaitingBeforeScript,
    WaitingScriptResult,
    WaitingAfterScript,
    Finished,
};

struct TestState {
    ETestPhase                             phase = ETestPhase::WaitingSceneReady;
    Moer::scripting::ScriptExecutionFuture script_future;
    Clock::time_point                      phase_start_time      = {};
    int                                    last_countdown_second = -1;
    int                                    exit_code             = 3;
    bool                                   exit_requested        = false;
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

bool NearlyEqual(float lhs, float rhs, float epsilon = k_float_epsilon) {
    return std::fabs(lhs - rhs) <= epsilon;
}

bool NearlyEqual(const Moer::float3& lhs, const Moer::float3& rhs, float epsilon = k_float_epsilon) {
    return NearlyEqual(lhs.x, rhs.x, epsilon) && NearlyEqual(lhs.y, rhs.y, epsilon) &&
           NearlyEqual(lhs.z, rhs.z, epsilon);
}

bool NearlyEqual(const Moer::Quaternion& lhs, const Moer::Quaternion& rhs, float epsilon = k_float_epsilon) {
    return NearlyEqual(lhs.vec.x, rhs.vec.x, epsilon) && NearlyEqual(lhs.vec.y, rhs.vec.y, epsilon) &&
           NearlyEqual(lhs.vec.z, rhs.vec.z, epsilon) && NearlyEqual(lhs.vec.w, rhs.vec.w, epsilon);
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

bool ValidateSceneState(Moer::Scene& scene) {
    const entt::entity root = scene.GetRootNodeEntity();
    if (!scene.IsValidNodeEntity(root)) {
        std::cout << "[cpp] Validation failed: root node is invalid." << std::endl;
        return false;
    }

    const std::string node_name = scene.GetNodeDisplayName(root);
    if (node_name != k_expected_name) {
        std::cout << "[cpp] Validation failed: node name mismatch. actual='" << node_name << "'" << std::endl;
        return false;
    }

    const auto local_transform = scene.TryGetNodeLocalTransform(root);
    if (!local_transform.has_value()) {
        std::cout << "[cpp] Validation failed: root local transform is unavailable." << std::endl;
        return false;
    }

    if (!NearlyEqual(local_transform->translation, k_expected_translation)) {
        std::cout << "[cpp] Validation failed: translation mismatch. actual="
                  << local_transform->translation.ToString(3) << std::endl;
        return false;
    }

    if (!NearlyEqual(local_transform->rotation, k_expected_rotation)) {
        std::cout << "[cpp] Validation failed: rotation mismatch. actual=(x="
                  << local_transform->rotation.vec.x << ", y=" << local_transform->rotation.vec.y
                  << ", z=" << local_transform->rotation.vec.z << ", w=" << local_transform->rotation.vec.w
                  << ")" << std::endl;
        return false;
    }

    if (!NearlyEqual(local_transform->scale, k_expected_scale)) {
        std::cout << "[cpp] Validation failed: scale mismatch. actual=" << local_transform->scale.ToString(3)
                  << std::endl;
        return false;
    }

    std::cout << "[cpp] Validation passed." << std::endl;
    return true;
}

} // namespace

int main(int argc, const char** argv) {
    std::cout << "Moer Engine Scripting Scene Control Test Starting..." << std::endl;

    const std::filesystem::path executable_dir = ResolveExecutableDir(argv[0]);
    const std::filesystem::path script_path =
        executable_dir / "test" / "scripting" / "scene_control_smoke.py";

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
                std::cout << "[cpp] Scene ready. Waiting 5 seconds before script execution." << std::endl;
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
                state.script_future = engine.SubmitScriptExecution(std::move(request));
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
                } else {
                    state.exit_code = ValidateSceneState(scene) ? 0 : 2;
                }

                state.phase                 = ETestPhase::WaitingAfterScript;
                state.phase_start_time      = Clock::now();
                state.last_countdown_second = -1;
                std::cout << "[cpp] Waiting 5 seconds before exit." << std::endl;
                return;
            }

            case ETestPhase::WaitingAfterScript: {
                LogCountdown(
                    "[cpp] Exit countdown: ",
                    state.phase_start_time,
                    k_after_script_delay,
                    state.last_countdown_second
                );

                if (Clock::now() - state.phase_start_time < k_after_script_delay) {
                    return;
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
