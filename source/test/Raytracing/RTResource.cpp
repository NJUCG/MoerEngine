#include "RTResource.h"
#include "config/ConfigManager.h"
#include <filesystem>

namespace Moer::Render {
    RTResource::RTResource(const std::filesystem::path& _resouce_path)
        : b_loaded(false), resource_path(_resouce_path) {
        //check valid path
        if (!std::filesystem::exists(_resouce_path)) {
            resource_path = ConfigManager::GetInstance().GetEditorResourcePath();
        }
    }

    RTResource::~RTResource() {
    }

    void RTResource::LoadResources() {
        //load resources

        //textures

        {
            auto texture_path = resource_path / "textures";
            if (std::filesystem::exists(texture_path)) {
                for (auto& entry : std::filesystem::directory_iterator(texture_path)) {
                }
            }
        }
    }

    void RTResource::UnloadResources() {
        //unload resources
    }

};// namespace Moer::Render