#include "Launcher.h"
#include "core/include/log/LogSystem.h"
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <stdint.h>
#include "Core.h"
int main(int argc, char** argv) {

    Moer::Launcher& launcher = Moer::Launcher::GetInstance();

    std::filesystem::path workspace = argv[0];

    launcher.Init(workspace.parent_path());

    try {
        launcher.Run();
    } catch (std::runtime_error& e) {
        LOG_ERROR("Engine Runtime error detected: {}", e.what());
    } catch (std::exception& e) {
        LOG_ERROR("Engine FATAL ERROR detected: {}", e.what());
    }

    launcher.Quit();
    return 0;
}
