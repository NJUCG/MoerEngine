// 实现配置文件加载，以及工作目录和各类资源路径的初始化。
#include "config/ConfigManager.h"

#include "log/LogSystem.h"
#include "misc/MacroUtils.h"

#include <filesystem>
#include <stdexcept>

// 构建系统会将 Shader 及共享头文件复制到资源目录，以下宏用于接收对应的相对路径。
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

void ConfigManager::Init(const std::filesystem::path& workspace_path) {
    InitInternal(workspace_path, workspace_path / "MoerEngine.toml", false);
}

void ConfigManager::Init(
    const std::filesystem::path& workspace_path,
    const std::filesystem::path& config_path
) {
    InitInternal(workspace_path, config_path, true);
}

void ConfigManager::InitInternal(
    const std::filesystem::path& workspace_path,
    const std::filesystem::path& config_path,
    bool                         is_override
) {
    m_workspace_path            = workspace_path;
    m_editor_resource_path      = workspace_path / "asset";
    m_engine_shader_path        = workspace_path / "asset" / MACRO_STR(SHADER_PATH_RELATIVE_TO_ASSET);
    m_engine_shader_cached_path = workspace_path / "asset" / "shader_cache";
    m_engine_shader_shared_path = workspace_path / "asset" / MACRO_STR(SHADER_SHARED_PATH_RELATIVE_TO_ASSET);

    const std::filesystem::path absolute_config_path = std::filesystem::absolute(config_path);
    if (is_override) {
        LOG_INFO("[Config] Using command-line config override: {}", absolute_config_path.generic_string());
    }
    if (!std::filesystem::exists(absolute_config_path)) {
        LOG_ERROR("Config `{}` does not exist.", absolute_config_path.generic_string());
        LOG_ERROR(
            "Please copy `template.MoerEngine.toml` to `MoerEngine.toml` in root directory. You can read "
            "README.md for details. MoerEngine will abort."
        );
        throw std::runtime_error("Config file does not exist");
    }

    m_config     = Config::GlobalConfig::LoadConfigFromTomlFile(absolute_config_path.generic_string());
    m_scene_path = m_config.engine.scene.scene_path;
    m_cache_path = workspace_path / "cache";
}

const std::filesystem::path& ConfigManager::GetWorkspacePath() const {
    return m_workspace_path;
}

const std::filesystem::path& ConfigManager::GetEditorResourcePath() const {
    return m_editor_resource_path;
}

const std::filesystem::path& ConfigManager::GetEngineShaderPath() const {
    return m_engine_shader_path;
}

const std::filesystem::path& ConfigManager::GetEngineShaderSharedPath() const {
    return m_engine_shader_shared_path;
}

const std::filesystem::path& ConfigManager::GetEngineShaderCachedPath() const {
    return m_engine_shader_cached_path;
}

const std::filesystem::path& ConfigManager::GetScenePath() const {
    return m_scene_path;
}

const std::filesystem::path& ConfigManager::GetCachePath() const {
    return m_cache_path;
}
} // namespace Moer
