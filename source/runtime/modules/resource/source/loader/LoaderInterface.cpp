#include "loader/LoaderInterface.h"
#include "ResourceAPI.h"
#include "loader/gltf/Parser.h"
#include "loader/ply/Ply.h"

#include <filesystem>
namespace Moer {
namespace Resource {
    class LoaderInterface {
    public:
        static RESOURCE_API UniquePtr<Scene> LoadSceneFromFile(const std::filesystem::path& file_path) noexcept {
            if (file_path.string().ends_with(".ply")) {
                return PlyLoader::LoadSceneFromFile(file_path);
            } else if (file_path.string().ends_with(".gltf")) {
                return Gltf::Parser::LoadSceneFromFile(file_path);
            } else {
                return nullptr;
            }
        }
    };
}
}