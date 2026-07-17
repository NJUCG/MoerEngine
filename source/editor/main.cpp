// 校验编辑器命令行参数，并管理顶层 Editor 生命周期。

#include "Editor.h"
#include "Engine.h"
#include "startup/StartupSplash.h"

#include <chrono>
#include <iostream>
#include <string_view>
#include <thread>
#include <utility>

namespace {

bool HasFlag(int argc, const char** argv, std::string_view flag) {
    for (int index = 1; index < argc; ++index) {
        if (std::string_view(argv[index]) == flag) {
            return true;
        }
    }
    return false;
}

} // namespace

int main(int argc, const char** argv) {
    std::cout << "Moer Engine Editor Starting..." << std::endl;

    try {
        Moer::Engine::ValidateCommandLine(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "Invalid command line: " << error.what() << std::endl;
        return 2;
    }

    Moer::StartupSplash splash;
    const bool splash_started =
        !HasFlag(argc, argv, "--no-splash") &&
        splash.Start("Starting MoerEditor", "Preparing engine services");

    Moer::Editor::StartupHooks startup_hooks{};
    startup_hooks.main_window_visible = !splash_started;
    if (splash_started) {
        startup_hooks.on_progress =
            [&splash](std::string_view title, std::string_view detail) {
                splash.Update(title, detail);
            };
        startup_hooks.on_first_main_present = [&splash]() {
            splash.Finish();
        };
    }

    Moer::Editor editor;
    try {
        editor.Init(argc, argv, std::move(startup_hooks));
        editor.Run();
        editor.ShutDown();
    } catch (const std::exception& error) {
        std::cerr << "MoerEditor startup failed: " << error.what() << std::endl;
        if (splash_started && splash.IsRunning()) {
            splash.Fail("MoerEditor failed to start", error.what());
        }
        editor.ShutDown();
        if (splash_started && splash.IsRunning()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        }
        return 1;
    } catch (...) {
        std::cerr << "MoerEditor startup failed with an unknown error." << std::endl;
        if (splash_started && splash.IsRunning()) {
            splash.Fail(
                "MoerEditor failed to start",
                "An unknown error interrupted engine initialization or the editor loop."
            );
        }
        editor.ShutDown();
        if (splash_started && splash.IsRunning()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        }
        return 1;
    }

    if (splash.IsRunning()) {
        splash.Finish();
    }

    return 0;
}
