#include "loader/LoaderInterface.h"
#include "scene/MaterialInstance.h"
#include "ResourceAPI.h"
#include "loader/gltf/Parser.h"
#include "loader/ply/Ply.h"
#include "loader/jsonscene/JsonSceneParser.h"
#include "log/LogSystem.h"
#include "scene/Scene.h"
#include "sceneCache/SceneCache.h"
#include "taskgraph/TaskGraph.h"

#include <filesystem>
namespace Moer {
namespace Resource {

    using LoadFunction                                                   = std::function<UniquePtr<SceneData>(const std::filesystem::path&)>;
    static Moer::Map<std::string, LoadFunction> scene_load_function_maps = {{"gltf", Gltf::Parser::LoadSceneFromFile},
                                                                            {"glb", Gltf::Parser::LoadSceneFromFile},
                                                                            {"fbx", Gltf::Parser::LoadSceneFromFile},
                                                                            {"obj", Gltf::Parser::LoadSceneFromFile},
                                                                            {"json", JsonScene::JsonSceneParser::LoadSceneFromFile}};

    void LoadFromFile(const std::filesystem::path& _file_path, Scene* _scene) {
        auto ext = _file_path.string().substr(_file_path.string().find_last_of(".") + 1);
        if (scene_load_function_maps.find(ext) == scene_load_function_maps.end()) {
            LOG_ERROR("Unsupported file format: {}", _file_path.extension().string());
            return;
        }
        AsyncSceneLoadInfoRef load_info = MoerNew(AsyncSceneLoadInfo)();
        load_info->b_valid              = true;
        load_info->progress.store(0);
        Scene::RegisterAsyncLoadInfo(load_info);
        LambdaTask::Dispatch([_file_path, _scene, load_info, ext]() {
            if (auto scene_data = std::move(scene_load_function_maps[ext](_file_path))) {
                LOG_INFO("Raw data is loaded to SceneData successfully, converting to Scene...");
                SceneCache::ConvertToScene(*scene_data, _scene, true);
                load_info->progress.store(1);
                LOG_INFO("Scene loaded successfully from file: {}", _file_path.string());
            }
        });
    }

    void LoaderInterface::LoadSceneFromFileAsync(const std::filesystem::path& _file_path, Scene* scene) noexcept {
        auto file_path_str = _file_path.string();
        LOG_INFO("Loading scene from file: {}", file_path_str);
        if (_file_path.string().ends_with(".ply")) {
            auto gs_scene = PlyLoader::LoadSceneFromFile(_file_path);
            scene->SetBuffer(EGpuSceneResource::GaussianSplattingVertex, gs_scene->GetBuffer(EGpuSceneResource::GaussianSplattingVertex));
        } else {
            if (SceneCache::HasValidCache(_file_path)) {
                LambdaTask::Dispatch([_file_path, scene]() {
                    try {
                        SceneCache::LoadSceneFromCache(_file_path, scene);
                        LOG_INFO("Scene loaded successfully from cache: {}", _file_path.string());
                    } catch (const std::exception& e) {
                        LOG_ERROR("Failed to load scene from cache: {} retrying to load from file", e.what());
                        Scene::ResetAsyncLoadInfo();
                        LoadFromFile(_file_path, scene);
                    }
                });
            } else {
                LoadFromFile(_file_path, scene);
            }
        }
    }
}
}// namespace Moer::Resource