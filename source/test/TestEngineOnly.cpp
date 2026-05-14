#include "Engine.h"

#include "scene/testcase/SceneTestSuiteRunner.h"

#include <iostream>

namespace {

struct TestEngineOnlyState {
    bool request_sent   = false;
    bool exit_requested = false;
    int  exit_code      = 2;
};

} // namespace

int main(int argc, const char** argv) {
    std::cout << "Moer Engine Engine-Only Test Starting..." << std::endl;

    Moer::Engine                engine;
    TestEngineOnlyState         state;
    Moer::SceneTestSuiteRunner& suite_runner = Moer::SceneTestSuiteRunner::Get();

    engine.Init(argc, argv);
    engine.GetEditorConfig()->selected_render_method = Moer::ERenderMethod::Raster;

    engine.Run(Moer::Render::EngineHooks{.on_tick_test = [&](Moer::Scene& scene) {
        if (state.exit_requested) {
            return;
        }

        return;

        if (!state.request_sent) {
            if (!scene.IsReady()) {
                return;
            }

            state.request_sent = true;
            if (!suite_runner.RequestRunAll(false, true)) {
                std::cerr << "Failed to start SceneTestSuiteRunner." << std::endl;
                state.exit_code      = 2;
                state.exit_requested = true;
                engine.RequestExit();
            }
            return;
        }

        if (suite_runner.IsRunning()) {
            return;
        }

        const auto& status = suite_runner.GetStatus();
        state.exit_code    = status.failed_case_count == 0 ? 0 : 1;

        std::cout << "Scene test suite finished: " << status.passed_case_count << "/"
                  << status.total_case_count << " passed, " << status.failed_case_count << " failed."
                  << std::endl;
        if (status.failed_case_count > 0 && !status.last_failed_case_name.empty()) {
            std::cout << "Last failed case: " << status.last_failed_case_name << std::endl;
            if (!status.last_failed_summary.empty()) {
                std::cout << "Failure summary: " << status.last_failed_summary << std::endl;
            }
        }

        state.exit_requested = true;
        engine.RequestExit();
    }});

    if (!state.exit_requested) {
        state.exit_code = 0;
    }

    engine.ShutDown();
    return state.exit_code;
}