#include "config/ConfigManager.h"

#include "log/LogSystem.h"
#include "config/ini.h"
#include "misc/MacroUtils.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <toml++/toml.hpp>

#ifndef DEVELOP_SHADER_PATH
#define DEVELOP_SHADER_PATH resource / shaders
#endif

#ifndef DEVELOP_SHADER_SHARED_PATH
#define DEVELOP_SHADER_SHARED_PATH resource / shaders
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
        engine_shader_path        = MACRO_STR(DEVELOP_SHADER_PATH);
        engine_shader_cached_path = _workspace_path / "resource" / "shader_cache";
        engine_shader_shared_path = MACRO_STR(DEVELOP_SHADER_SHARED_PATH);

        // check config exists
        std::filesystem::path config_path = _workspace_path / CONFIG_DIR / "MoerEngine.toml";
        if (!std::filesystem::exists(config_path)) {
            LOG_ERROR("Config `MoerEngine.toml` does not exist.");
            LOG_ERROR("Please enter `./source/configs/` and copy `template.MoerEngine.toml` to `MoerEngine.toml`. You can read README.md for details. MoerEngine will abort.", config_path.generic_string());
            throw std::runtime_error("Config file does not exist");
        }

        // load config from .toml
        m_config   = Config::GlobalConfig::LoadConfigFromTomlFile(config_path.generic_string());
        scene_path = m_config.engine.scene.scene_path;

        LOG_INFO("RHI Config Name: {}", m_config.engine.rhi.rhi_config_name);
        LOG_INFO("Default RHI: {}", m_config.engine.rhi.default_rhi);
        LOG_INFO("Default render method: {}", m_config.engine.render.default_render_method);
        LOG_INFO("scene path: {}", m_config.engine.scene.scene_path);

        // load rhi configs from .json
        std::filesystem::path rhi_config_path = _workspace_path / CONFIG_DIR / m_config.engine.rhi.rhi_config_name;
        if (!std::filesystem::exists(rhi_config_path)) {
            throw std::runtime_error("RHIConfig directory does not exist");
        }
        m_rhi_config_as_json = nlohmann::json::parse(std::ifstream(rhi_config_path.generic_string()));
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
}// namespace Moer
