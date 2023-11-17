#include <filesystem>
#include <fstream>
#include <sstream>
// #include <string.h>
#include <string>
#include "config/ConfigManager.h"

#include "config/ini.h"
namespace Moer {
    ConfigManager& ConfigManager::GetInstance() {
        static ConfigManager instance;
        return instance;
    }

    void ConfigManager::Init(const std::filesystem::path& _workspace_path) {
        workspace_path                    = _workspace_path;
        std::filesystem::path config_path = _workspace_path / CONFIG_DIR / "MoerEngine.ini";
        if (!std::filesystem::exists(config_path)) {
            throw std::runtime_error("Config directory does not exist");
        }

        //load init config to init_config by ini.h
        inih::INIReader r{config_path.generic_string()};
        if (r.ParseError() < 0) {
            throw std::runtime_error("Can't load 'MoerEditor.ini'");
        }
#if defined(EDITOR_MODE_ON)
        init_config.editor_width           = r.Get<int>("editor", "editor_width", 1920);
        init_config.editor_height          = r.Get<int>("editor", "editor_height", 1080);
        init_config.editor_fullscreen      = r.Get<int>("editor", "editor_fullscreen", 0);
        init_config.editor_vsync           = r.Get<int>("editor", "editor_vsync", 1);
        init_config.editor_lock_frame_rate = r.Get<int>("editor", "editor_lock_frame_rate", 0);
        init_config.editor_fps             = r.Get<int>("editor", "editor_fps", 60);
        init_config.editor_max_fps         = r.Get<int>("editor", "editor_max_fps", 120);
        init_config.editor_font_size       = r.Get<float>("editor", "editor_font_size", 16.f);
#endif
        init_config.max_frame_in_flight = r.Get<int>("engine", "max_frame_in_flight", 3);

        auto default_rhi = r.Get<std::string>("engine", "default_rhi", "Vulkan");

        strcpy_s(init_config.default_rhi, default_rhi.c_str());
    }

    // std::string ConfigManager::GetConfig(const std::string& key) {
    //     return configs[key];
    // }

    const std::filesystem::path ConfigManager::GetWorkspacePath() const {
        return workspace_path;
    }

    const std::filesystem::path ConfigManager::GetEditorResourcePath() const {
        return workspace_path / "resource";
    }
}// namespace Moer
