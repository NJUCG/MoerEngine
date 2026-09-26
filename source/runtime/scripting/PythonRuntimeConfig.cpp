#include "scripting/PythonRuntimeConfig.h"

#include "config/ConfigManager.h"

namespace Moer::scripting {

PythonRuntimeConfig PythonRuntimeConfig::Default() {
    const std::filesystem::path runtime_root = ConfigManager::GetInstance().GetWorkspacePath();

    PythonRuntimeConfig config;
    config.runtime_root = runtime_root;
#if defined(_WIN32)
    config.program_path = runtime_root / "MoerEditor.exe";
    config.stdlib_dir   = runtime_root / "Lib";
    config.dll_dir      = runtime_root / "DLLs";
#else
    config.program_path = runtime_root / "MoerEditor";
    config.stdlib_dir   = MOER_PYTHON_STDLIB_DIR;
    config.dll_dir      = MOER_PYTHON_DYNLOAD_DIR;
#endif
    return config;
}

} // namespace Moer::scripting
