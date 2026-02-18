#include "LoaderInterface.h"

#include "log/LogSystem.h"
#include "scene/loader/assimp/Parser.h"
#include "taskgraph/TaskGraph.h"
#include <cassert>
#include <filesystem>

namespace Moer {

using LoadFunction = std::function<bool(ecs::LogicalScene&, const std::filesystem::path&)>;
static Moer::Map<std::string, LoadFunction> scene_load_function_maps = {
    {"gltf", assimp::Parser::LoadSceneFromFile},
    {"glb", assimp::Parser::LoadSceneFromFile},
    {"fbx", assimp::Parser::LoadSceneFromFile},
    {"obj", assimp::Parser::LoadSceneFromFile},
    {"dae", assimp::Parser::LoadSceneFromFile},
};

bool LoaderInterface::LoadSceneFromFile(
    ecs::LogicalScene&           out_logical_scene,
    const std::filesystem::path& file_path
) {
    LOG_INFO("Start loading LogicalScene from file: {}", file_path.string());

    return LoaderInterface::LoadSceneFromFileCommon(out_logical_scene, file_path);
}

// 在目前实现中，我们不调用这个接口，而是Scene类同时异步加载LogicalScene, CpuScene和GpuScene
// - 三个Scene一起异步，而不是此函数只异步LogicalScene
SharedPtr<SceneLoadInfoAsync> LoaderInterface::LoadSceneFromFileAsync(
    ecs::LogicalScene&           out_logical_scene,
    const std::filesystem::path& file_path
) {
    LOG_INFO("Start loading LogicalScene from file \"Async\": {}", file_path.string());

    SharedPtr<SceneLoadInfoAsync> load_info = MakeShared<SceneLoadInfoAsync>();

    LambdaTask::Dispatch([load_info, &out_logical_scene, file_path]() {
        load_info->StartLoading();

        bool result = LoaderInterface::LoadSceneFromFileCommon(out_logical_scene, file_path);

        if (result) {
            load_info->FinishLoading();

        } else {
            load_info->Reset();
        }
    });

    return load_info;
}

bool LoaderInterface::LoadSceneFromFileCommon(
    ecs::LogicalScene&           out_logical_scene,
    const std::filesystem::path& file_path
) {
    // TODO: cache
    // bool is_enabled_cache = ConfigManager::GetInstance().GetConfig().engine.scene.enable_cache;

    auto ext = file_path.extension().string().substr(1); // ".gltf" -> "gltf"
    // auto ext = file_path.string().substr(_file_path.string().find_last_of(".") + 1);

    if (scene_load_function_maps.contains(ext) == false) {
        LOG_ERROR("Loading Logical Scene - Unsupported file format: {}", ext);
        return false;
    }

    bool result = scene_load_function_maps[ext](out_logical_scene, file_path);

    if (!result) {
        LOG_ERROR("Loading Logical Scene - Failed to load scene from file: {}", file_path.string());
        return false;
    }

    LOG_INFO("Loading Logical Scene - Scene loaded successfully from file: {}", file_path.string());
    return true;
}

} // namespace Moer