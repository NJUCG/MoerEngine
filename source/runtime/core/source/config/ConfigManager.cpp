#include "config/ConfigManager.h"

#include "config/ini.h"
#include "log/LogSystem.h"
#include "misc/MacroUtils.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <toml++/toml.hpp>

// 在编译期，构建系统会对资源进行拷贝
// 这两个宏是为了接收构建系统拷贝的目标路径
#ifndef SHADER_PATH_RELATIVE_TO_RESOURCE
#define SHADER_PATH_RELATIVE_TO_RESOURCE shaders
#endif

#ifndef SHADER_SHARED_PATH_RELATIVE_TO_RESOURCE
#define SHADER_SHARED_PATH_RELATIVE_TO_RESOURCE shaderheaders
#endif

namespace Moer {
ConfigManager& ConfigManager::GetInstance() {
    static ConfigManager instance;
    return instance;
}

void ConfigManager::Init(const std::filesystem::path& _workspace_path) {
    // pathes
    workspace_path            = _workspace_path;
    editor_resource_path      = _workspace_path / "resource";
    engine_shader_path        = _workspace_path / "resource" / MACRO_STR(SHADER_PATH_RELATIVE_TO_RESOURCE);
    engine_shader_cached_path = _workspace_path / "resource" / "shader_cache";
    engine_shader_shared_path =
        _workspace_path / "resource" / MACRO_STR(SHADER_SHARED_PATH_RELATIVE_TO_RESOURCE);

    // check config exists
    std::filesystem::path config_path = _workspace_path / CONFIG_DIR / "MoerEngine.toml";
    if (!std::filesystem::exists(config_path)) {
        LOG_ERROR("Config `MoerEngine.toml` does not exist.");
        LOG_ERROR(
            "Please enter `./source/configs/` and copy `template.MoerEngine.toml` to `MoerEngine.toml`. You "
            "can read README.md for details. MoerEngine will abort.",
            config_path.generic_string()
        );
        throw std::runtime_error("Config file does not exist");
    }

    // load config from .toml
    m_config   = Config::GlobalConfig::LoadConfigFromTomlFile(config_path.generic_string());
    scene_path = m_config.engine.scene.scene_path;
    cache_path = _workspace_path / "cache";
}

const std::filesystem::path& ConfigManager::GetWorkspacePath() const {
    return workspace_path;
}

const std::filesystem::path& ConfigManager::GetEditorResourcePath() const {
    return editor_resource_path;
}

const std::filesystem::path& ConfigManager::GetEngineShaderPath() const {
    return engine_shader_path;
}

const std::filesystem::path& ConfigManager::GetEngineShaderSharedPath() const {
    return engine_shader_shared_path;
}

const std::filesystem::path& ConfigManager::GetEngineShaderCachedPath() const {
    return engine_shader_cached_path;
}

const std::filesystem::path& ConfigManager::GetScenePath() const {
    return scene_path;
}

const std::filesystem::path& ConfigManager::GetCachePath() const {
    return cache_path;
}
} // namespace Moer
