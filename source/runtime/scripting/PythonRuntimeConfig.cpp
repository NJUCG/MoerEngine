#include "scripting/PythonRuntimeConfig.h"

#include "config/ConfigManager.h"

namespace Moer::scripting {

PythonRuntimeConfig PythonRuntimeConfig::Default() {
    const std::filesystem::path runtime_root = ConfigManager::GetInstance().GetWorkspacePath();

    PythonRuntimeConfig config;
    config.runtime_root = runtime_root;
    config.program_path = runtime_root / "MoerEditor.exe";
    config.stdlib_dir   = runtime_root / "Lib";
    config.dll_dir      = runtime_root / "DLLs";
    return config;
}

} // namespace Moer::scripting