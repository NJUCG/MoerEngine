#include "Engine.h"
#include <filesystem>

int main(int argc, const char** argv) {

    Moer::Engine         engine;
    Moer::EngineInitInfo info{std::filesystem::path(argv[0])};

    engine.Init(info);
    engine.PostInit();
    engine.Run();
    engine.Quit();
}