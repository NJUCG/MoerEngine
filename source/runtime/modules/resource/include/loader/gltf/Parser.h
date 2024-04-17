#pragma once

#include <future>
#include <string>
#include <memory>
#include <filesystem>
#include "ResourceAPI.h"

#include <scene/Scene.h>

namespace Moer::Resource::Gltf {

    class Parser {
    public: 
        Parser() noexcept;
        ~Parser() noexcept;

        static RESOURCE_API UniquePtr<Scene> LoadSceneFromFile(const std::filesystem::path& file_path) noexcept;
        static RESOURCE_API void             LoadSceneFromFileAsync(const std::filesystem::path& file_path) noexcept;

    private:
        struct Impl;
        Impl* m_impl = nullptr;
    };
}// namespace Moer::Resource::Gltf 