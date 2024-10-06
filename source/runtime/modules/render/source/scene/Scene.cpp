#include "scene/Scene.h"

#include "config/ConfigManager.h"
// #include "loader/gltf/Parser.h"
#include "log/LogSystem.h"
#include "rhi/RHIResource.h"
#include "scene/EntityManager.h"
#include "scene/RenderableManager.h"
#include "rhi/RHI.h"
#include <atomic>

namespace Moer {
    // Scene * Scene::default_scene = nullptr;
    Scene* g_scene = nullptr;
    struct GpuScene {
        RHIBufferRef GetGpuBuffer(EGpuSceneResource _resource) const { return global_resources.buffers[(uint32_t)_resource]; }
        RHIRayTracingTLASRef GetTLAS() const { return tlas; }
    private:
        struct GResource {
            StaticArray<RHIBufferRef, (uint32_t)EGpuSceneResource::Num> buffers;
        } global_resources;
        RHIRayTracingTLASRef tlas;
    };
    class RENDER_API Scene::Impl {
        friend class Scene;

    public:
        void         AddEntity(Entity _entity) noexcept { m_entities.emplace(_entity); }
        void         AddCamera(Entity _entity) noexcept { m_cameras.emplace(_entity); }
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
        static AsyncSceneLoadInfoRef GetCurrentSceneLoadInfo() noexcept { return m_load_info; }

        GpuScene& GetGpuScene() noexcept { return gpu_scene; }

    protected:
        Map<std::string, RHIBufferRef> m_buffers;
        Map<std::string, RHIUAVRef>    m_uavs;
        Map<std::string, RHISRVRef>    m_srvs;

        EntitySet m_entities;
        EntitySet m_cameras;

        static AsyncSceneLoadInfoRef m_load_info;
        GpuScene                     gpu_scene;
    };
    AsyncSceneLoadInfoRef Scene::Impl::m_load_info{nullptr};

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

    Scene::Scene() noexcept {
        m_impl = new Impl();
    }

    Scene::~Scene() noexcept {
        delete m_impl;
    }

    void Scene::AddEntity(Entity entity) noexcept {
        m_impl->AddEntity(entity);
    }
    void Scene::RemoveEntity(Entity entity) noexcept {
        m_impl->RemoveEntity(entity);
    }
    void Scene::SetBuffer(const std::string& name, RHIBufferRef buffer) noexcept {
        return m_impl->SetBuffer(name, buffer);
    }
    RHIBufferRef Scene::GetBuffer(const std::string& name) const noexcept {
        return m_impl->GetBuffer(name);
    }

    void Scene::AddCamera(Entity entity) noexcept {
        m_impl->AddCamera(entity);
    }

    Array<Entity> Scene::GetEntities() const noexcept {
        return m_impl->GetEntities();
    }

    Array<Entity> Scene::GetCameras() const noexcept {
        return m_impl->GetCameras();
    }
    Entity Scene::GetMainCamera() const noexcept {
        return GetCameras()[0];
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

    GpuScene& Scene::GetGpuScene() noexcept {
        return m_impl->GetGpuScene();
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

    Scene* AsyncSceneLoadInfo::TryGetScene() {
        if (progress.load(std::memory_order_acq_rel) == 1) {
            return scene;
        }
        return nullptr;
    }

}// namespace Moer