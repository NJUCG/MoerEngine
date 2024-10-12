#pragma once
#include "ResourceAPI.h"
#include "misc/STL.h"
#include "scene/Scene.h"
#include "scene/SceneData.h"

namespace Moer {

    class SceneCache {
    public:
        static RESOURCE_API UniquePtr<Scene> FromFile(const std::filesystem::path& path);
        static RESOURCE_API bool             HasValidCache(const std::filesystem::path& path);
        static RESOURCE_API void             ToFile(const Scene& scene, const std::filesystem::path& path);
        static RESOURCE_API UniquePtr<Scene> ConvertToScene(SceneData& scene_data, bool need_cache = true);
        static RESOURCE_API void             LoadSceneFromCacheAsync(const std::filesystem::path& path);
        //
    protected:
        class InputStream;
        class OutputStream;

        static RESOURCE_API void ReadSceneGeomInfo(InputStream& stream, SceneData& sceneData);
        static RESOURCE_API void ReadSceneMaterial(InputStream& stream, SceneData& sceneData);
        static RESOURCE_API void ReadSceneTextures(InputStream& stream, SceneData& sceneData);
        static RESOURCE_API void ReadSceneUtils(InputStream& stream, SceneData& sceneData);

        static RESOURCE_API void WriteSceneGeomInfo(OutputStream& stream, const SceneData& sceneData);
        static RESOURCE_API void WriteSceneMaterial(OutputStream& stream, const SceneData& sceneData);
        static RESOURCE_API void WriteSceneTextures(OutputStream& stream, const SceneData& sceneData);
        static RESOURCE_API void WriteSceneUtils(OutputStream& stream, const SceneData& sceneData);

        static RESOURCE_API SceneData ConvertToSceneData(const Scene& scene);
        static RESOURCE_API void      Cache(const SceneData& sceneData, size_t key);
    };
}