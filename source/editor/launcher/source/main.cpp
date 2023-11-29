#include "Launcher.h"
#include "core/include/log/LogSystem.h"
#include <corecrt_malloc.h>
#include <exception>
#include <filesystem>
#include "Core.h"

#if defined(_WIN32) || defined(_WIN64)
// #include <mimalloc-new-delete.h>
#include <windows.h>

HINSTANCE hDLL;// Handle to DLL
HRESULT   LoadMiMalloc() {

    HRESULT hrReturnVal;

    hDLL = LoadLibrary("mimalloc.dll");
    return hrReturnVal;
}
HRESULT result = LoadMiMalloc();

HRESULT
UnloadMiMalloc() {

    HRESULT hrReturnVal;

    FreeLibrary(hDLL);
    return hrReturnVal;
}

#endif

// static int mv = mi_version();

int main(int argc, char** argv) {
    int32_t* kk = (int*)malloc(100 * sizeof(int32_t));
    free(kk);
    Moer::Launcher& launcher = Moer::Launcher::GetInstance();

    std::filesystem::path workspace = argv[0];

    launcher.Init(workspace.parent_path());

    try {
        launcher.Run();
    } catch (std::exception& e) {
        e.what();
        LOG_ERROR("Engine FATAL ERROR detected.");
    }

    launcher.Quit();
#if PLATFORM_WINDOWS
    HRESULT h = UnloadMiMalloc();
#endif
    return 0;
}
