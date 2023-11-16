#include <filesystem>
#include <fstream>
#include <sstream>
#include "config/ConfigManager.h"

#include "config/ini.h"
namespace Moer {
    ConfigManager& ConfigManager::GetInstance() {
        static ConfigManager instance;
        return instance;
    }

    void ConfigManager::Init(const std::filesystem::path& _workspace_path) {
        workspace_path                    = _workspace_path;
        std::filesystem::path config_path = _workspace_path / "config" / "MoerEngine.ini";
        if (!std::filesystem::exists(config_path)) {
            throw std::runtime_error("Config directory does not exist");
        }

        //load init config to init_config by ini.h
        inih::INIReader r{config_path.generic_string()};
        if (r.ParseError() < 0) {
            throw std::runtime_error("Can't load 'MoerEditor.ini'");
        }

        init_config.editor_width           = r.Get<int>("editor", "editor_width", 1920);
        init_config.editor_height          = r.Get<int>("editor", "editor_height", 1080);
        init_config.editor_fullscreen      = r.Get<int>("editor", "editor_fullscreen", 0);
        init_config.editor_vsync           = r.Get<int>("editor", "editor_vsync", 1);
        init_config.editor_lock_frame_rate = r.Get<int>("editor", "editor_lock_frame_rate", 0);
        init_config.editor_fps             = r.Get<int>("editor", "editor_fps", 60);
        init_config.editor_max_fps         = r.Get<int>("editor", "editor_max_fps", 120);
        init_config.editor_font_size       = r.Get<float>("editor", "editor_font_size", 16.f);

        // if (!std::filesystem::exists(config_path)) {
        //     throw std::runtime_error("Config directory does not exist");
        // }

        // for (const auto& entry : std::filesystem::directory_iterator(config_path)) {
        //     if (entry.is_regular_file()) {
        //         std::ifstream config_file(entry.path());
        //         if (config_file.is_open()) {
        //             std::stringstream buffer;
        //             buffer << config_file.rdbuf();
        //             std::string config_content                = buffer.str();
        //             configs[entry.path().filename().string()] = config_content;
        //         }
        //     }
        // }
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
