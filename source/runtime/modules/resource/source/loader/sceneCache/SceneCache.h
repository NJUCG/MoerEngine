#pragma once
#include "ResourceAPI.h"
#include "misc/STL.h"
#include "scene/Scene.h"
#include "scene/SceneData.h"
#include "serialize/Serializer.h"
#include <fstream>

namespace Moer {

    class SceneCache {
    public:
        using FInputStream  = Moer::InputStream;
        using FOutputStream = Moer::OutputStream;

        static RESOURCE_API void FromFile(const std::filesystem::path& path, Scene* scene);
        static RESOURCE_API bool HasValidCache(const std::filesystem::path& path);
        static RESOURCE_API void ToFile(const Scene& scene, const std::filesystem::path& path);
        static RESOURCE_API void ConvertToScene(SceneData& scene_data, Scene* scene, bool need_cache = true);
        static RESOURCE_API void LoadSceneFromCacheAsync(const std::filesystem::path& path, Scene* scene);
        static RESOURCE_API void LoadSceneFromCache(const std::filesystem::path& path, Scene* scene);
        //
    protected:
        static RESOURCE_API void ReadSceneGeomInfo(FInputStream& stream, SceneData& sceneData);
        static RESOURCE_API void ReadSceneMaterial(FInputStream& stream, SceneData& sceneData);
        static RESOURCE_API void ReadSceneTextures(FInputStream& stream, SceneData& sceneData);
        static RESOURCE_API void ReadSceneUtils(FInputStream& stream, SceneData& sceneData);

        static RESOURCE_API void WriteSceneGeomInfo(FOutputStream& stream, const SceneData& sceneData);
        static RESOURCE_API void WriteSceneMaterial(FOutputStream& stream, const SceneData& sceneData);
        static RESOURCE_API void WriteSceneTextures(FOutputStream& stream, const SceneData& sceneData);
        static RESOURCE_API void WriteSceneUtils(FOutputStream& stream, const SceneData& sceneData);

        static RESOURCE_API SceneData ConvertToSceneData(const Scene& scene);
        static RESOURCE_API void      Cache(const SceneData& sceneData, size_t key);
    };
}// namespace Moer