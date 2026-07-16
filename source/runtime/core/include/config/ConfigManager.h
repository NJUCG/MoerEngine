// 管理全局配置，并集中提供运行时所需的工作目录与资源路径。
#ifndef MOER_ENGINE_CONFIG_MANAGER_H
#define MOER_ENGINE_CONFIG_MANAGER_H

#include "API_Macro.h"
#include "config/GlobalConfig.h"

#include <filesystem>

// UI 字体资源相对于 asset 根目录的位置；保留宏以兼容现有调用方。
#define FONTS_DIR "fonts"
namespace Moer {

class CORE_API ConfigManager {

public:
    static ConfigManager& GetInstance();

    void Init(const std::filesystem::path& workspace_path);
    void Init(
        const std::filesystem::path& workspace_path,
        const std::filesystem::path& config_path
    );

    const std::filesystem::path& GetWorkspacePath() const;
    const std::filesystem::path& GetEditorResourcePath() const;
    const std::filesystem::path& GetEngineShaderPath() const;
    const std::filesystem::path& GetEngineShaderSharedPath() const;
    const std::filesystem::path& GetEngineShaderCachedPath() const;
    const std::filesystem::path& GetScenePath() const;
    const std::filesystem::path& GetCachePath() const;

    // 仅在配置管理器完成初始化后调用。
    const Config::GlobalConfig& GetConfig() const {
        return m_config;
    }

private:
    ConfigManager() = default;

    void InitInternal(
        const std::filesystem::path& workspace_path,
        const std::filesystem::path& config_path,
        bool                         is_override
    );

    Config::GlobalConfig m_config;

    std::filesystem::path m_workspace_path;
    std::filesystem::path m_editor_resource_path;
    std::filesystem::path m_engine_shader_path;
    std::filesystem::path m_engine_shader_shared_path;
    std::filesystem::path m_engine_shader_cached_path;
    std::filesystem::path m_scene_path;
    std::filesystem::path m_cache_path;
};

} // namespace Moer

#endif // MOER_ENGINE_CONFIG_MANAGER_H
