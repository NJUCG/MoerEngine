#pragma once
#include "ResourceAPI.h"
#include "misc/STL.h"
#include "scene/Scene.h"

#include <filesystem>
namespace Moer {

namespace Moer::Resource {
    class LoaderInterface {
    public:
        static RESOURCE_API UniquePtr<Scene> LoadSceneFromFile(const std::filesystem::path& file_path) noexcept;
    };
}

}