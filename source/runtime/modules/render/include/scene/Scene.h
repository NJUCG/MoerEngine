#pragma once
#include <future>
#include <unordered_set>
#include <vector>

#include "API_Macro.h"
#include "Entity.h"
#include "math/Base.h"
#include "math/Matrix.h"
#include "misc/CountableRef.h"
#include "rhi/RHIResource.h"

namespace Moer {
    enum class EGpuSceneResource {
        MeshInfo,
        InstanceInfo,
        MaterialInfo,
        LightInfo,
        CameraInfo,
        Num
    };
    struct InstanceData {
        Moer::Matrix4x4f model2world;
        Moer::Matrix4x4f inv_model2world;
        float            scale;
        uint32_t         padding;
        uint32_t         material_id;
        uint32_t         material_type;
    };

    struct InstanceMeshInfo {
        Vector3f center;
        uint32_t vertex_offset;
        Vector3f extent;
        uint32_t vertex_count;
        uint32_t index_offset;
        uint32_t index_count;
        uint32_t meshlet_offset;
        uint32_t meshlet_count;
    };

    using EntitySet = Moer::UnorderedSet<Entity, Entity::Hasher>;

    struct AsyncSceneLoadInfo {

        bool         IsValid() const noexcept { return b_valid; };
        bool         IsReady() const noexcept { return b_valid && progress == 1.f; };
        float        GetProgress() const noexcept { return progress.load(); };
        class Scene* TryGetScene();
        COUNTABLE_IMPLEMENTATION_AUTO_DESTROY
        // private:
        Scene*           scene;
        std::atomic_uint progress = 0u;
        bool             b_valid  = false;
    };
    using AsyncSceneLoadInfoRef = CountableRef<AsyncSceneLoadInfo>;
#define DEFAULT_SCENE_NAME "Sponza"
    struct GpuScene;
    class RENDER_API Scene {
    public:
        Scene() noexcept;
        ~Scene() noexcept;
        void          AddEntity(Entity _entity) noexcept;
        void          AddCamera(Entity _entity) noexcept;
        void          AddLight(Entity _entity) noexcept;
        void          SetTlas(RHIRayTracingTLASRef _tlas) noexcept;
        void SetBlasList(Moer::Array<RHIRayTracingBLASRef> _blas_list) noexcept;
        void SetRaytracingScene(Render::RaytracingSceneRef _scene) noexcept;
        void          RemoveEntity(Entity _entity) noexcept;
        void          SetBuffer(const std::string& _name, RHIBufferRef _buffer) noexcept;
        RHIBufferRef  GetBuffer(const std::string& _name) const noexcept;
        Array<Entity> GetEntities() const noexcept;
        Array<Entity> GetLights() const noexcept;
        Array<Entity> GetCameras() const noexcept;
        Entity        GetMainCamera() const noexcept;
        bool          IsEntitiesEmpty() const noexcept;
        bool          IsLightsEmpty() const noexcept;
        bool          IsCamerasEmpty() const noexcept;
        void          ForEach(std::function<void(Entity)> _func) const noexcept;
        bool          IsReady() const noexcept;

        static Scene* GetCurrentScene() noexcept;
        static void   SetCurrentScene(Scene* _scene) noexcept;

        static AsyncSceneLoadInfoRef GetCurrentSceneLoadInfo() noexcept;

        static bool RegisterAsyncLoadInfo(AsyncSceneLoadInfoRef _load_info);

        GpuScene& GetGpuScene() noexcept;

        void SetVertexBuffer(Render::BufferRef _buffer) noexcept;
        void SetIndexBuffer(Render::BufferRef _buffer) noexcept;

        Render::BufferRef GetVertexBuffer() const noexcept;
        Render::BufferRef GetIndexBuffer() const noexcept;
        

    protected:
        class Impl;
        Impl* m_impl = nullptr;
    };

    extern RENDER_API Scene* g_scene;

}// namespace Moer