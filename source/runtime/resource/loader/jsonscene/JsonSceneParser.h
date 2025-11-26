#pragma once
#include "ResourceAPI.h"
#include "scene/SceneData.h"
#include <filesystem>
#include <future>
#include <memory>
#include <scene/Scene.h>
#include <string>
namespace Moer::Resource::JsonScene {
using Path = std::filesystem::path;
class JsonSceneParser {
public:
    JsonSceneParser() noexcept;
    ~JsonSceneParser() noexcept;

    static RESOURCE_API UniquePtr<SceneData> LoadSceneFromFile(const Path& abs_scn_json_path) noexcept;

private:
    class Impl;
    Impl* m_impl = nullptr;
};
} // namespace Moer::Resource::JsonScene