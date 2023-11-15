#ifndef MOER_ENGINE_CONFIG_MANAGER_H
#define MOER_ENGINE_CONFIG_MANAGER_H
#include <filesystem>
#include <unordered_map>
#include "API_Macro.h"
//implement ConfigManager as Singleton
#define FONTS_DIR "fonts"
namespace Moer {
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
    };
}// namespace Moer

#endif//MOER_ENGINE_CONFIG_MANAGER_H