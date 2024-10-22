#include "scene/Scene.h"

#include "config/ConfigManager.h"
// #include "loader/gltf/Parser.h"
#include "log/LogSystem.h"
#include "misc/STL.h"
#include "rhi/RHIResource.h"
#include "scene/EntityManager.h"
#include "scene/RenderableManager.h"
#include "rhi/RHI.h"
#include <atomic>

namespace Moer {
    // Scene * Scene::default_scene = nullptr;
    Scene* g_scene = nullptr;
    struct GpuScene {
        Render::BufferRef GetGpuBuffer(EGpuSceneResource _resource) const { return global_resources.buffers[(uint32_t)_resource]; }
        // RHIRayTracingTLASRef GetTLAS() const { return tlas; }
        struct GResource {
            StaticArray<Render::BufferRef, (uint32_t)EGpuSceneResource::Num> buffers;
        } global_resources;
        Render::RaytracingSceneRef rt_scene{nullptr};
        Render::BufferRef          vertex_buffer{nullptr}, index_buffer{nullptr};
        Render::BindlessArrayRef   bindless_array{nullptr};

        UnorderedMap<std::string, Render::TextureRef> material_textures;
    };
    class RENDER_API Scene::Impl {
        friend class Scene;

    public:
        Impl() noexcept;
        void         AddEntity(Entity _entity) noexcept { m_entities.emplace(_entity); }
        void         AddCamera(Entity _entity) noexcept { m_cameras.emplace(_entity); }
        void         AddLight(Entity _entity) noexcept { m_lights.emplace(_entity); }
        void         RemoveEntity(Entity _entity) noexcept { m_entities.erase(_entity); };
        void         SetBuffer(const std::string& _name, RHIBufferRef _buffer) { m_buffers[_name] = _buffer; }
        RHIBufferRef GetBuffer(const std::string& _name) const { return m_buffers.at(_name); }
        RHIUAVRef    GetUAV(const std::string& _name) const { return m_uavs.at(_name); }
        RHISRVRef    GetSRV(const std::string& _name) const { return m_srvs.at(_name); }
        void         ForEach(std::function<void(Entity)> _func) const noexcept {
            for (auto& entity : m_entities) {
                _func(entity);
            }
        }
        Array<Entity>                GetEntities() const noexcept;
        Array<Entity>                GetCameras() const noexcept;
        Array<Entity>                GetLights() const noexcept;
        bool                         IsEntitiesEmpty() const noexcept { return m_entities.empty(); }
        bool                         IsCamerasEmpty() const noexcept { return m_cameras.empty(); }
        bool                         IsLightsEmpty() const noexcept { return m_lights.empty(); }
        static AsyncSceneLoadInfoRef GetCurrentSceneLoadInfo() noexcept { return m_load_info; }

        GpuScene& GetGpuScene() noexcept { return gpu_scene; }

    protected:
        Map<std::string, RHIBufferRef> m_buffers;
        Map<std::string, RHIUAVRef>    m_uavs;
        Map<std::string, RHISRVRef>    m_srvs;

        EntitySet m_entities;
        EntitySet m_cameras;
        EntitySet m_lights;

        static AsyncSceneLoadInfoRef m_load_info;
        GpuScene                     gpu_scene;
    };
    AsyncSceneLoadInfoRef Scene::Impl::m_load_info{nullptr};

    Scene::Impl::Impl() noexcept {
        gpu_scene.bindless_array = Render::RenderDevice::Get().CreateBindlessArray();
    }
    Array<Entity> Scene::Impl::GetEntities() const noexcept {
        Array<Entity> result;
        // result.reserve(m_entities.size());
        for (const Entity& entity : m_entities) {

            result.push_back(entity);
        }
        return result;
    }

    Array<Entity> Scene::Impl::GetCameras() const noexcept {
        Array<Entity> result;
        result.reserve(m_cameras.size());
        for (auto& entity : m_cameras) {
            result.push_back(entity);
        }
        return result;
    }

    Array<Entity> Scene::Impl::GetLights() const noexcept {
        Array<Entity> result;
        result.reserve(m_lights.size());
        for (auto& entity : m_lights) {
            result.push_back(entity);
        }
        return result;
    }

    Scene::Scene() noexcept {
        m_impl = new Impl();
    }

    Scene::~Scene() noexcept {
        delete m_impl;
    }

    void Scene::AddEntity(Entity entity) noexcept {
        m_impl->AddEntity(entity);
    }
    void Scene::SetBlasList(Moer::Array<RHIRayTracingBLASRef> _blas_list) noexcept {
        // m_impl->gpu_scene.blases =  std::move(_blas_list);
    }
    void Scene::SetRaytracingScene(Render::RaytracingSceneRef _scene) noexcept {
        m_impl->gpu_scene.rt_scene = _scene;
    }

    void Scene::SetTlas(RHIRayTracingTLASRef _tlas) noexcept {
        // m_impl->gpu_scene.tlas = _tlas;
    }
    void Scene::RemoveEntity(Entity entity) noexcept {
        m_impl->RemoveEntity(entity);
    }
    void Scene::SetBuffer(const std::string& name, RHIBufferRef buffer) noexcept {
        return m_impl->SetBuffer(name, buffer);
    }
    void Scene::SetBuffer(EGpuSceneResource _type, Render::BufferRef _buffer) noexcept {
        m_impl->gpu_scene.global_resources.buffers[(uint32_t)_type] = _buffer;
    }
    Render::BufferRef Scene::GetBuffer(EGpuSceneResource _type) const noexcept {
        return m_impl->gpu_scene.global_resources.buffers[(uint32_t)_type];
    }
    RHIBufferRef Scene::GetBuffer(const std::string& _name) const noexcept {
        return nullptr;
    }

    void Scene::AddCamera(Entity entity) noexcept {
        m_impl->AddCamera(entity);
    }

    void Scene::AddLight(Entity entity) noexcept {
        m_impl->AddLight(entity);
    }

    Array<Entity> Scene::GetEntities() const noexcept {
        return m_impl->GetEntities();
    }

    Array<Entity> Scene::GetLights() const noexcept {
        return m_impl->GetLights();
    }

    Array<Entity> Scene::GetCameras() const noexcept {
        return m_impl->GetCameras();
    }

    Entity Scene::GetMainCamera() const noexcept {
        return GetCameras()[0];
    }

    bool Scene::IsEntitiesEmpty() const noexcept {
        return m_impl->IsEntitiesEmpty();
    }

    bool Scene::IsLightsEmpty() const noexcept {
        return m_impl->IsLightsEmpty();
    }

    bool Scene::IsCamerasEmpty() const noexcept {
        return m_impl->IsCamerasEmpty();
    }

    void Scene::ForEach(std::function<void(Entity)> _func) const noexcept {
        m_impl->ForEach(std::move(_func));
    }

    Scene* Scene::GetCurrentScene() noexcept {
        return g_scene;
    }
    void Scene::SetCurrentScene(Scene* _scene) noexcept {
        g_scene = _scene;
    }

    bool Scene::IsReady() const noexcept {
        return true;
    }

    void Scene::RegisterMaterialTextures(UnorderedMap<std::string, Render::TextureRef> _textures) noexcept {
        m_impl->gpu_scene.material_textures.insert(_textures.begin(), _textures.end());
    }

    GpuScene& Scene::GetGpuScene() noexcept {
        return m_impl->GetGpuScene();
    }
    void Scene::SetVertexBuffer(Render::BufferRef _buffer) noexcept {
        m_impl->gpu_scene.vertex_buffer = _buffer;
    }

    void Scene::SetIndexBuffer(Render::BufferRef _buffer) noexcept {
        m_impl->gpu_scene.index_buffer = _buffer;
    }
    void Scene::SetInstanceBuffer(Render::BufferRef _buffer) noexcept {
        m_impl->gpu_scene.global_resources.buffers[(uint32_t)EGpuSceneResource::InstanceInfo] = _buffer;
    }
    Render::BufferRef Scene::GetVertexBuffer() const noexcept {
        return m_impl->gpu_scene.vertex_buffer;
    }
    Render::BufferRef Scene::GetIndexBuffer() const noexcept {
        return m_impl->gpu_scene.index_buffer;
    }
    Render::BufferRef Scene::GetInstanceBuffer() const noexcept {
        return m_impl->gpu_scene.GetGpuBuffer(EGpuSceneResource::InstanceInfo);
    }
    Render::BindlessArrayRef Scene::GetBindlessArray() const noexcept {
        return m_impl->gpu_scene.bindless_array;
    }

    AsyncSceneLoadInfoRef Scene::GetCurrentSceneLoadInfo() noexcept {
        return Impl::GetCurrentSceneLoadInfo();
    }

    bool Scene::RegisterAsyncLoadInfo(AsyncSceneLoadInfoRef _load_info) {
        if (Impl::m_load_info) {
            if (Impl::m_load_info.IsValid() && !Impl::m_load_info->IsReady()) {
                LOG_ERROR("Scene is already loading");
                return false;
            }
            //TODO: release current_scene
        }
        Impl::m_load_info = _load_info;
        return true;
    }

    void Scene::ResetAsyncLoadInfo() noexcept {
        Impl::m_load_info = nullptr;
    }

    Scene* AsyncSceneLoadInfo::TryGetScene() {
        if (progress.load(std::memory_order_acq_rel) == 1) {
            return scene;
        }
        return nullptr;
    }

}// namespace Moer