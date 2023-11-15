#include <filesystem>
#include <fstream>
#include <sstream>
#include "config/ConfigManager.h"
namespace Moer {
    ConfigManager& ConfigManager::GetInstance() {
        static ConfigManager instance;
        return instance;
    }

    void ConfigManager::Init(const std::filesystem::path& _workspace_path) {
        workspace_path = _workspace_path;
        // std::filesystem::path config_path = _workspace_path / "config";
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
