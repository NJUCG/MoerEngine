#pragma once
#include <future>
#include <string>
#include <memory>
#include <filesystem>
#include "ResourceAPI.h"
#include "scene/SceneData.h"
#include <scene/Scene.h>
namespace Moer::Resource::JsonScene {
    using Path = std::filesystem::path;
    class JsonSceneParser {
    public:
        JsonSceneParser() noexcept;
        ~JsonSceneParser() noexcept;

        static RESOURCE_API UniquePtr<SceneData> LoadSceneFromFile(const Path& abs_scn_json_path) noexcept;
        static RESOURCE_API void                 LoadSceneFromFileAsync(const Path& abs_scn_json_path) noexcept;

    private:
        class Impl;
        Impl* m_impl = nullptr;
    };
}// namespace Moer::Resource::JsonScene