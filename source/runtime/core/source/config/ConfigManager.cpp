#include "config/ConfigManager.h"

#include "config/CVarSystem.h"
#include "log/LogSystem.h"
#include "misc/MacroUtils.h"

#include <cstdint>
#include <filesystem>
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
namespace {

UnorderedMap<std::string, std::string>
LoadConsoleVariableMapFromTomlFile(const std::filesystem::path& config_path) {
    UnorderedMap<std::string, std::string> values;

    auto config = toml::parse_file(config_path.generic_string());
    values.reserve(config.size());
    for (const auto& [key, node] : config) {
        const std::string key_text = std::string(key.str());
        if (const auto value = node.value<std::string>()) {
            values[key_text] = *value;
            continue;
        }
        if (const auto value = node.value<bool>()) {
            values[key_text] = *value ? "true" : "false";
            continue;
        }
        if (const auto value = node.value<int64_t>()) {
            values[key_text] = std::to_string(*value);
            continue;
        }
        if (const auto value = node.value<double>()) {
            values[key_text] = std::to_string(*value);
            continue;
        }

        LOG_WARNING(
            "Ignore unsupported cvar override `{}` from {}. Only string, bool, int and float are supported.",
            key_text,
            config_path.generic_string()
        );
    }

    return values;
}

} // namespace

ConfigManager& ConfigManager::GetInstance() {
    static ConfigManager instance;
    return instance;
}

void ConfigManager::Init(const std::filesystem::path& _workspace_path) {
    // pathes
    workspace_path            = _workspace_path;
    editor_resource_path      = _workspace_path / "asset";
    engine_shader_path        = _workspace_path / "asset" / MACRO_STR(SHADER_PATH_RELATIVE_TO_ASSET);
    engine_shader_cached_path = _workspace_path / "asset" / "shader_cache";
    engine_shader_shared_path = _workspace_path / "asset" / MACRO_STR(SHADER_SHARED_PATH_RELATIVE_TO_ASSET);

    // check config exists
    std::filesystem::path config_path = _workspace_path / "MoerEngine.toml";
    if (!std::filesystem::exists(config_path)) {
        LOG_ERROR("Config `{}` does not exist.", config_path.generic_string());
        LOG_ERROR(
            "Please copy `template.MoerEngine.toml` to `MoerEngine.toml` in root directory. You can read "
            "README.md for details. MoerEngine will abort."
        );
        throw std::runtime_error("Config file does not exist");
    }

    std::filesystem::path cvar_config_path = _workspace_path / "ConsoleVariable.toml";
    if (!std::filesystem::exists(cvar_config_path)) {
        LOG_ERROR("Config `{}` does not exist.", cvar_config_path.generic_string());
        LOG_ERROR(
            "Please copy `template.ConsoleVariable.toml` to `ConsoleVariable.toml` in root directory. "
            "MoerEngine will abort."
        );
        throw std::runtime_error("Console variable config file does not exist");
    }

    // load config from .toml
    m_config   = Config::GlobalConfig::LoadConfigFromTomlFile(config_path.generic_string());
    scene_path = m_config.engine.scene.scene_path;
    cache_path = _workspace_path / "cache";

    CVar::ApplyValueMap(
        LoadConsoleVariableMapFromTomlFile(cvar_config_path),
        cvar_config_path.filename().generic_string()
    );
    CVar::SealStartupConfigReadOnlyCVars();
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
