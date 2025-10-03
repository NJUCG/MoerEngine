#ifndef MOER_ENGINE_CONFIG_MANAGER_H
#define MOER_ENGINE_CONFIG_MANAGER_H

#include "API_Macro.h"
#include "config/GlobalConfig.h"
#include "misc/STL.h"

#include <filesystem>

//implement ConfigManager as Singleton
#define FONTS_DIR  "fonts"
#define CONFIG_DIR "config"
namespace Moer {

class CORE_API ConfigManager {

public:
    static ConfigManager& GetInstance();

    void Init(const std::filesystem::path& _workspace_path);

    const std::filesystem::path& GetWorkspacePath() const;
    const std::filesystem::path& GetEditorResourcePath() const;
    const std::filesystem::path& GetEngineShaderPath() const;
    const std::filesystem::path& GetEngineShaderSharedPath() const;
    const std::filesystem::path& GetEngineShaderCachedPath() const;
    const std::filesystem::path& GetScenePath() const;
    const std::filesystem::path& GetCachePath() const;

    //call after config manager init
    const Config::GlobalConfig& GetConfig() const {
        return m_config;
    }

private:
    static ConfigManager* instance;

    ConfigManager() {}

private:
    Config::GlobalConfig m_config;

    std::filesystem::path workspace_path;
    std::filesystem::path editor_resource_path;
    std::filesystem::path engine_shader_path;
    std::filesystem::path engine_shader_shared_path;
    std::filesystem::path engine_shader_cached_path;
    std::filesystem::path scene_path;
    std::filesystem::path cache_path;
};

} // namespace Moer

#endif //MOER_ENGINE_CONFIG_MANAGER_H