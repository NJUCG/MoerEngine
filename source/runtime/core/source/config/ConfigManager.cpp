// 实现配置文件加载，以及工作目录和各类资源路径的初始化。
#include "config/ConfigManager.h"

#include "log/LogSystem.h"
#include "misc/MacroUtils.h"

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

// 构建系统会将 Shader 及共享头文件复制到资源目录，以下宏用于接收对应的相对路径。
#ifndef SHADER_PATH_RELATIVE_TO_ASSET
#define SHADER_PATH_RELATIVE_TO_ASSET shaders
#endif

#ifndef SHADER_SHARED_PATH_RELATIVE_TO_ASSET
#define SHADER_SHARED_PATH_RELATIVE_TO_ASSET shaderheaders
#endif

namespace Moer {

namespace {

std::filesystem::path MakeAbsoluteNormalized(const std::filesystem::path& path) {
    std::error_code       error;
    std::filesystem::path absolute_path = std::filesystem::absolute(path, error);
    if (error) {
        return path.lexically_normal();
    }

    std::filesystem::path canonical_path = std::filesystem::weakly_canonical(absolute_path, error);
    return error ? absolute_path.lexically_normal() : canonical_path;
}

std::filesystem::path ResolveWorkspacePath(
    const std::filesystem::path& workspace_path,
    const std::filesystem::path& configured_path
) {
    if (configured_path.empty()) {
        return {};
    }
    return MakeAbsoluteNormalized(
        configured_path.is_relative() ? workspace_path / configured_path : configured_path
    );
}

#if defined(_WIN32)
std::optional<std::filesystem::path> ReadEnvironmentPath(const wchar_t* name) {
    const DWORD required_size = GetEnvironmentVariableW(name, nullptr, 0);
    if (required_size == 0) {
        return std::nullopt;
    }

    std::vector<wchar_t> value(required_size);
    if (GetEnvironmentVariableW(name, value.data(), required_size) == 0 || value.front() == L'\0') {
        return std::nullopt;
    }
    return std::filesystem::path(value.data());
}
#else
std::optional<std::filesystem::path> ReadEnvironmentPath(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return std::nullopt;
    }
    return std::filesystem::path(value);
}
#endif

bool EnsureDirectory(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::create_directories(path, error);
    return !error && std::filesystem::is_directory(path, error) && !error;
}

std::filesystem::path ResolveEditorSettingsPath(const std::filesystem::path& workspace_path) {
#if defined(_WIN32)
    const auto override_path = ReadEnvironmentPath(L"MOER_EDITOR_SETTINGS_DIR");
#else
    const auto override_path = ReadEnvironmentPath("MOER_EDITOR_SETTINGS_DIR");
#endif
    if (override_path) {
        const std::filesystem::path resolved = ResolveWorkspacePath(workspace_path, *override_path);
        if (EnsureDirectory(resolved)) {
            return resolved;
        }
        LOG_WARNING(
            "[EditorSettings] Unable to create override directory `{}`; falling back.", resolved.string()
        );
    }

    const std::filesystem::path project_settings =
        MakeAbsoluteNormalized(workspace_path / "saved" / "editor");
    if (!EnsureDirectory(project_settings)) {
        LOG_WARNING(
            "[EditorSettings] Unable to create project-local directory `{}`. "
            "Editor state will not persist.",
            project_settings.string()
        );
    }
    return project_settings;
}

} // namespace

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
    m_workspace_path            = MakeAbsoluteNormalized(workspace_path);
    m_editor_resource_path      = m_workspace_path / "asset";
    m_engine_shader_path        = m_workspace_path / "asset" / MACRO_STR(SHADER_PATH_RELATIVE_TO_ASSET);
    m_engine_shader_cached_path = m_workspace_path / "asset" / "shader_cache";
    m_engine_shader_shared_path =
        m_workspace_path / "asset" / MACRO_STR(SHADER_SHARED_PATH_RELATIVE_TO_ASSET);

    const std::filesystem::path absolute_config_path = MakeAbsoluteNormalized(config_path);
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

    m_config = Config::GlobalConfig::LoadConfigFromTomlFile(absolute_config_path.generic_string());

    const std::filesystem::path preset_imgui_path =
        ResolveWorkspacePath(m_workspace_path, m_config.editor.preset_imgui_config_path);
    m_config.editor.preset_imgui_config_path = preset_imgui_path.generic_string();

    m_scene_path = ResolveWorkspacePath(m_workspace_path, m_config.engine.scene.scene_path);
    m_config.engine.scene.scene_path = m_scene_path.generic_string();

    m_cache_path           = m_workspace_path / "cache";
    m_editor_settings_path = ResolveEditorSettingsPath(m_workspace_path);
    LOG_INFO("[EditorSettings] Directory : {}", m_editor_settings_path.string());
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

const std::filesystem::path& ConfigManager::GetEditorSettingsPath() const {
    return m_editor_settings_path;
}
} // namespace Moer
