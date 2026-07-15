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
#ifndef SHADER_PATH_RELATIVE_TO_ASSET
#define SHADER_PATH_RELATIVE_TO_ASSET shaders
#endif

#ifndef SHADER_SHARED_PATH_RELATIVE_TO_ASSET
#define SHADER_SHARED_PATH_RELATIVE_TO_ASSET shaderheaders
#endif

namespace Moer {
ConfigManager& ConfigManager::GetInstance() {
    static ConfigManager instance;
    return instance;
}

void ConfigManager::Init(const std::filesystem::path& _workspace_path) {
    InitInternal(_workspace_path, _workspace_path / "MoerEngine.toml", false);
}

void ConfigManager::Init(
    const std::filesystem::path& _workspace_path,
    const std::filesystem::path& _config_path
) {
    InitInternal(_workspace_path, _config_path, true);
}

void ConfigManager::InitInternal(
    const std::filesystem::path& _workspace_path,
    const std::filesystem::path& _config_path,
    bool                         _is_override
) {
    // pathes
    workspace_path            = _workspace_path;
    editor_resource_path      = _workspace_path / "asset";
    engine_shader_path        = _workspace_path / "asset" / MACRO_STR(SHADER_PATH_RELATIVE_TO_ASSET);
    engine_shader_cached_path = _workspace_path / "asset" / "shader_cache";
    engine_shader_shared_path = _workspace_path / "asset" / MACRO_STR(SHADER_SHARED_PATH_RELATIVE_TO_ASSET);

    // check config exists
    const std::filesystem::path config_path = std::filesystem::absolute(_config_path);
    if (_is_override) {
        LOG_INFO("[Config] Using command-line config override: {}", config_path.generic_string());
    }
    if (!std::filesystem::exists(config_path)) {
        LOG_ERROR("Config `{}` does not exist.", config_path.generic_string());
        LOG_ERROR(
            "Please copy `template.MoerEngine.toml` to `MoerEngine.toml` in root directory. You can read "
            "README.md for details. MoerEngine will abort."
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
