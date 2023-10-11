#include "Launcher.h"
#include "log/LogSystem.h"
#include <exception>
#include <filesystem>

int main(int argc, char** argv) {

    Moer::Launcher& launcher = Moer::Launcher::GetInstance();

    std::filesystem::path workspace = argv[0];

    launcher.Init(workspace);

    try {
        launcher.Run();
    } catch (std::exception& e) {
        e.what();
        LOG_ERROR("Engine FATAL ERROR detected.");
    }

    launcher.Quit();

    return 0;
}