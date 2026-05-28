#include "LoaderInterface.h"

#include "config/ConfigManager.h"
#include "log/LogSystem.h"
#include "scene/LogicalScene.h"
#include "scene/cache/SceneCache.h"
#include "scene/cache/SceneCacheSerializer.h"
#include "scene/loader/assimp/Parser.h"
#include <chrono>
#include <entt/entt.hpp>
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

namespace {

double ElapsedMs(std::chrono::steady_clock::time_point start_time) {
    const auto elapsed = std::chrono::steady_clock::now() - start_time;
    return std::chrono::duration<double, std::milli>(elapsed).count();
}

void ClearLogicalScene(ecs::LogicalScene& logical_scene) {
    logical_scene.r() = entt::registry{};
}

bool TryLoadSceneCache(
    ecs::LogicalScene&              out_logical_scene,
    const SceneCacheSourceIdentity& source_identity,
    ESceneCacheKind                 cache_kind,
    std::filesystem::path&          out_cache_path
) {
    out_cache_path = SceneCache::GetCachePath(source_identity, cache_kind);

    SceneCacheHeader cache_header{};
    if (!SceneCache::IsCacheValid(out_cache_path, source_identity, cache_kind, &cache_header)) {
        return false;
    }

    const auto cache_read_start = std::chrono::steady_clock::now();
    if (SceneCacheSerializer::LoadLogicalScene(out_cache_path, out_logical_scene, &cache_header)) {
        LOG_INFO(
            "Scene {} Cache hit: path={}, read_time_ms={:.2f}, payload_size={} bytes",
            SceneCache::KindToString(cache_kind),
            out_cache_path.string(),
            ElapsedMs(cache_read_start),
            cache_header.payload_size
        );
        return true;
    }

    LOG_WARNING(
        "Scene {} Cache read failed, falling back to parser: path={}",
        SceneCache::KindToString(cache_kind),
        out_cache_path.string()
    );
    ClearLogicalScene(out_logical_scene);
    return false;
}

ESceneLoadSource ToLoadSource(ESceneCacheKind cache_kind) {
    switch (cache_kind) {
        case ESceneCacheKind::State:
            return ESceneLoadSource::StateCache;
        case ESceneCacheKind::Origin:
            return ESceneLoadSource::OriginCache;
    }
    return ESceneLoadSource::None;
}

} // namespace

SceneImportResult LoaderInterface::LoadScene(const SceneLoadRequest& request) {
    SceneImportResult result{};
    result.logical_scene = MakeUnique<ecs::LogicalScene>();

    ESceneLoadSource source = ESceneLoadSource::None;
    result.success = LoaderInterface::LoadSceneFromFileCommon(*result.logical_scene, request, &source);
    result.source  = source;

    if (!result.success) {
        result.logical_scene.reset();
    }

    return result;
}

bool LoaderInterface::LoadSceneFromFileCommon(
    ecs::LogicalScene&      out_logical_scene,
    const SceneLoadRequest& request,
    ESceneLoadSource*       out_source
) {
    const std::filesystem::path& file_path = request.file_path;
    if (out_source) {
        *out_source = ESceneLoadSource::None;
    }

    auto ext = file_path.extension().string().substr(1); // ".gltf" -> "gltf"
    // auto ext = file_path.string().substr(_file_path.string().find_last_of(".") + 1);

    if (scene_load_function_maps.contains(ext) == false) {
        LOG_ERROR(
            "Loading Logical Scene - Unsupported file format: path={}, ext={}",
            file_path.string(),
            ext.empty() ? "<none>" : ext
        );
        return false;
    }

    const bool is_cache_enabled = ConfigManager::GetInstance().GetConfig().engine.scene.enable_cache;

    bool                     has_source_identity = false;
    SceneCacheSourceIdentity source_identity{};
    std::filesystem::path    origin_cache_path;

    if (is_cache_enabled) {
        has_source_identity = SceneCache::BuildSourceIdentity(file_path, source_identity);
        if (has_source_identity) {
            if (request.use_state_cache) {
                std::filesystem::path state_cache_path;
                if (TryLoadSceneCache(
                        out_logical_scene, source_identity, ESceneCacheKind::State, state_cache_path
                    )) {
                    if (out_source) {
                        *out_source = ToLoadSource(ESceneCacheKind::State);
                    }
                    return true;
                }
            }
            if (request.use_origin_cache) {
                if (TryLoadSceneCache(
                        out_logical_scene, source_identity, ESceneCacheKind::Origin, origin_cache_path
                    )) {
                    if (out_source) {
                        *out_source = ToLoadSource(ESceneCacheKind::Origin);
                    }
                    return true;
                }
            }
        }
    }

    const auto parser_start = std::chrono::steady_clock::now();

    // Load data from original file using parser
    bool result = scene_load_function_maps[ext](out_logical_scene, file_path);

    if (!result) {
        LOG_ERROR("Loading Logical Scene - Failed to load scene from file: {}", file_path.string());
        return false;
    }

    if (out_source) {
        *out_source = ESceneLoadSource::Parser;
    }

    LOG_INFO(
        "Loading Logical Scene - Scene loaded successfully from file: {}, parser_time_ms={:.2f}",
        file_path.string(),
        ElapsedMs(parser_start)
    );

    if (is_cache_enabled && has_source_identity && request.allow_write_origin_cache) {
        const auto cache_write_start   = std::chrono::steady_clock::now();
        origin_cache_path              = SceneCache::GetCachePath(source_identity, ESceneCacheKind::Origin);
        SceneCacheHeader origin_header = SceneCache::CreateHeader(source_identity, ESceneCacheKind::Origin);
        if (SceneCacheSerializer::SaveLogicalScene(origin_cache_path, origin_header, out_logical_scene)) {
            LOG_INFO(
                "Scene Origin Cache written: path={}, write_time_ms={:.2f}",
                origin_cache_path.string(),
                ElapsedMs(cache_write_start)
            );
        } else {
            LOG_WARNING("Scene Origin Cache write failed: path={}", origin_cache_path.string());
        }
    }

    return true;
}

} // namespace Moer