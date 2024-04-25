#pragma once
#include <string>
#include <memory>
#include <filesystem>
#include "ResourceAPI.h"

#include <scene/Scene.h>

namespace Moer::Resource {
    class PlyLoader {
    public:
        static RESOURCE_API UniquePtr<Scene> LoadSceneFromFile(const std::filesystem::path& file_path) noexcept;

    protected:
        class Impl;
        Impl* impl{nullptr};
    };
}