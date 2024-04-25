#include "loader/LoaderInterface.h"
#include "ResourceAPI.h"
#include "loader/gltf/Parser.h"
#include "loader/ply/Ply.h"
#include "log/LogSystem.h"

#include <filesystem>
namespace Moer {
    namespace Resource {
        void LoaderInterface::LoadSceneFromFileAsync(const std::filesystem::path& _file_path) noexcept {
            auto file_path_str = _file_path.string();
            if (_file_path.string().ends_with(".ply")) {
                auto scene = PlyLoader::LoadSceneFromFile(_file_path);
                if (g_scene != nullptr) {
                    g_scene->SetBuffer("gs_scene_buffer", scene->GetBuffer("gs_scene_buffer"));
                } else {
                    Scene::SetCurrentScene(scene.release());
                }
            } else if (_file_path.string().ends_with(".gltf")) {
                Gltf::Parser::LoadSceneFromFileAsync(_file_path);
            } else {
                LOG_ERROR("Unsupported file format: {}", file_path_str);
            }
        }
    }
}// namespace Moer::Resource