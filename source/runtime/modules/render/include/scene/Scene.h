#pragma once
#include <future>
#include <unordered_set>
#include <vector>

#include "API_Macro.h"
#include "Entity.h"
#include "math/Base.h"
#include "math/Matrix.h"
#include "misc/CountableRef.h"
#include "misc/STL.h"
#include "rhi/RHIResource.h"

namespace Moer {
    enum class EGpuSceneResource {
        MeshInfo,
        InstanceInfo,
        MaterialInfo,
        LightInfo,
        CameraInfo,
        GaussianSplattingVertex,
        RTInstance,
        RTPrimitive,
        RTVertex,
        RTMeshInfo,
        Num
    };
    struct InstanceData {
        Matrix4x4f model2world;
        Matrix4x4f inv_model2world;
        float      scale;
        uint       padding;
        uint       material_id;
        uint       material_type;
    };

    struct RTInstance {
        float4   overload_m1;
        float4   overload_m2;
        float4   overload_m3;
        uint32_t material_id;
        uint32_t material_type;
        uint     prim_offset;
        uint     vtx_offset;
    };

    struct RTMeshInfo {
        uint32_t vertex_offset;
        uint32_t vertex_count;
        uint32_t primitive_offset;
        uint32_t primitive_count;
    };

    struct RTVertex {
        float3 position;
        float  uv0;
        float3 normal;
        float  uv1;
        float3 tangent;
        float  padding;
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
        void              AddEntity(Entity _entity) noexcept;
        void              AddCamera(Entity _entity) noexcept;
        void              AddLight(Entity _entity) noexcept;
        void              SetTlas(RHIRayTracingTLASRef _tlas) noexcept;
        void              SetBlasList(Moer::Array<RHIRayTracingBLASRef> _blas_list) noexcept;
        void              SetRaytracingScene(Render::RaytracingSceneRef _scene) noexcept;
        void              RemoveEntity(Entity _entity) noexcept;
        void              SetBuffer(const std::string& _name, RHIBufferRef _buffer) noexcept;
        void              SetBuffer(EGpuSceneResource _type, Render::BufferRef _buffer) noexcept;
        Render::BufferRef GetBuffer(EGpuSceneResource _type) const noexcept;
        RHIBufferRef      GetBuffer(const std::string& _name) const noexcept;
        Array<Entity>     GetEntities() const noexcept;
        Array<Entity>     GetLights() const noexcept;
        Array<Entity>     GetCameras() const noexcept;
        Entity            GetMainCamera() const noexcept;
        bool              IsEntitiesEmpty() const noexcept;
        bool              IsLightsEmpty() const noexcept;
        bool              IsCamerasEmpty() const noexcept;
        void              ForEach(std::function<void(Entity)> _func) const noexcept;
        bool              IsReady() const noexcept;

        // [temperory]
        void RegisterMaterialTextures(UnorderedMap<std::string, Render::TextureRef> _textures) noexcept;

        static Scene* GetCurrentScene() noexcept;
        static void   SetCurrentScene(Scene* _scene) noexcept;

        static AsyncSceneLoadInfoRef GetCurrentSceneLoadInfo() noexcept;

        static bool RegisterAsyncLoadInfo(AsyncSceneLoadInfoRef _load_info);
        static void ResetAsyncLoadInfo() noexcept;

        GpuScene& GetGpuScene() noexcept;

        void SetVertexBuffer(Render::BufferRef _buffer) noexcept;
        void SetIndexBuffer(Render::BufferRef _buffer) noexcept;
        void SetInstanceBuffer(Render::BufferRef _buffer) noexcept;

        Render::BufferRef        GetVertexBuffer() const noexcept;
        Render::BufferRef        GetIndexBuffer() const noexcept;
        Render::BufferRef        GetInstanceBuffer() const noexcept;
        Render::BindlessArrayRef GetBindlessArray() const noexcept;

    protected:
        class Impl;
        Impl* m_impl = nullptr;
    };

    extern RENDER_API Scene* g_scene;

}// namespace Moer