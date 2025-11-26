#pragma once
#include "ResourceAPI.h"
#include <filesystem>
#include <memory>
#include <string>

#include <scene/Scene.h>

namespace Moer::Resource {
class PlyLoader {
public:
    static RESOURCE_API UniquePtr<Scene> LoadSceneFromFile(const std::filesystem::path& file_path) noexcept;

protected:
    class Impl;
    Impl* impl{nullptr};
};
} // namespace Moer::Resource