#ifndef MOER_ENGINE_CONFIG_MANAGER_H
#define MOER_ENGINE_CONFIG_MANAGER_H
#include <filesystem>
#include <unordered_map>
#include "API_Macro.h"
//implement ConfigManager as Singleton
#define FONTS_DIR "fonts"
namespace Moer {
    struct MoerInitConfig {
        uint32_t editor_width;
        uint32_t editor_height;
        uint32_t editor_fullscreen : 1;
        uint32_t editor_vsync : 1;
        uint32_t editor_lock_frame_rate : 1;
        uint32_t editor_fps : 8;
        uint32_t editor_max_fps : 8;

        float editor_font_size{16.f};
    };
    class CORE_API ConfigManager {
    private:
        static ConfigManager* instance;

        std::unordered_map<std::string, std::string> configs;

        std::filesystem::path workspace_path;
        ConfigManager() {}

    public:
        static ConfigManager& GetInstance();

        void Init(const std::filesystem::path& workspacePath);

        // std::string GetConfig(const std::string& key);

        const std::filesystem::path GetWorkspacePath() const;

        const std::filesystem::path GetEditorResourcePath() const;

        //call after config manager init
        const MoerInitConfig& GetInitConfig() const { return init_config; }

    private:
        MoerInitConfig init_config;
    };
}// namespace Moer

#endif//MOER_ENGINE_CONFIG_MANAGER_H