#ifndef MOER_ENGINE_CONFIG_MANAGER_H
#define MOER_ENGINE_CONFIG_MANAGER_H
#include <filesystem>

#include "API_Macro.h"
#include "misc/STL.h"

#include <nlohmann/json.hpp>
using MoerRHIConfigAsJSON = nlohmann::json;

//implement ConfigManager as Singleton
#define FONTS_DIR  "fonts"
#define CONFIG_DIR "config"
namespace Moer {
    struct MoerInitConfig {
#if defined(EDITOR_MODE_ON)
        //EDITOR CONFIGS
        uint32_t editor_width;
        uint32_t editor_height;
        uint32_t editor_fullscreen : 1;
        uint32_t editor_vsync : 1;
        uint32_t editor_lock_frame_rate : 1;
        uint32_t editor_fps : 8;
        uint32_t editor_max_fps : 8;

        float editor_font_size{16.f};
#endif
        //ENGINE CONFIGS
        uint32_t max_frame_in_flight : 3;
        bool     ray_tracing : 1;

        char rhi_config_name[128]{"VkConfigs.json"};
        char default_rhi[32]{"Vulkan"};
        char default_render_name[32]{"DeferredRenderer"};
    };
    class CORE_API ConfigManager {
    private:
        static ConfigManager* instance;

        Moer::UnorderedMap<std::string, std::string> configs;

        std::filesystem::path workspace_path;
        std::filesystem::path editor_resource_path;
        std::filesystem::path engine_shader_path;
        std::filesystem::path engine_shader_cached_path;
        std::filesystem::path scene_path;
        ConfigManager() {}

    public:
        static ConfigManager& GetInstance();

        void Init(const std::filesystem::path& workspacePath);

        // std::string GetConfig(const std::string& key);

        const std::filesystem::path& GetWorkspacePath() const;

        const std::filesystem::path& GetEditorResourcePath() const;

        const std::filesystem::path& GetEngineShaderPath() const;

        const std::filesystem::path& GetEngineShaderCachedPath() const;

        const std::filesystem::path& GetScenePath() const;

        //call after config manager init
        const MoerInitConfig&      GetInitConfig() const { return init_config; }
        const MoerRHIConfigAsJSON& GetRHIConfigAsJSON() const { return rhi_config_as_json; }

    private:
        MoerInitConfig      init_config;
        MoerRHIConfigAsJSON rhi_config_as_json;
    };

}// namespace Moer

#endif//MOER_ENGINE_CONFIG_MANAGER_H