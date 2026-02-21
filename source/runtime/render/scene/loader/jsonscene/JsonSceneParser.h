// #pragma once
// #include "RenderAPI.h"
// #include "scene/SceneData.h"
// #include <filesystem>
// #include <future>
// #include <memory>
// #include <scene/Scene.h>
// #include <string>
// namespace Moer::JsonScene {
// using Path = std::filesystem::path;
// class JsonSceneParser {
// public:
//     JsonSceneParser() noexcept;
//     ~JsonSceneParser() noexcept;

//     static RENDER_API UniquePtr<SceneData> LoadSceneFromFile(const Path& abs_scn_json_path) noexcept;

// private:
//     class Impl;
//     Impl* m_impl = nullptr;
// };
// } // namespace Moer::JsonScene