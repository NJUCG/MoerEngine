#include "Launcher.h"
#include <exception>
#include <filesystem>

int main(int argc, char** argv) {
#if VULKAN
    return 1;
#endif

    Launcher& launcher = Launcher::GetInstance();

    std::filesystem::path workspace = argv[0];

    launcher.Init(workspace);

    try {
        launcher.Run();
    } catch (std::exception& e) {
        e.what();
    }

    launcher.Quit();


    return 0;
}