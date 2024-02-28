#include "scene/Scene.h"

#include "config/ConfigManager.h"
// #include "loader/gltf/Parser.h"
#include "rhi/RHIResource.h"
#include "scene/EntityManager.h"
#include "scene/RenderableManager.h"
#include "rhi/RHI.h"

namespace Moer {
    // Scene * Scene::default_scene = nullptr;
    Scene* g_scene = nullptr;

    class RENDER_API Scene::Impl {
    public:
        void         AddEntity(Entity entity) noexcept { m_entities.emplace(entity); }
        void         AddCamera(Entity entity) noexcept { m_cameras.emplace(entity); }
        void         RemoveEntity(Entity entity) noexcept { m_entities.erase(entity); };
        void         SetBuffer(const std::string& name, RHIBufferRef buffer) { m_buffers[name] = buffer; }
        RHIBufferRef GetBuffer(const std::string& name) const { return m_buffers.at(name); }
        RHIUAVRef    GetUAV(const std::string& name) const { return m_uavs.at(name); }
        RHISRVRef    GetSRV(const std::string& name) const { return m_srvs.at(name); }
        void         ForEach(std::function<void(Entity)> func) const noexcept {
            for (auto& entity : m_entities) {
                func(entity);
            }
        }
        Array<Entity> GetEntities() const noexcept;
        Array<Entity> GetCameras() const noexcept;

    protected:
        Map<std::string, RHIBufferRef> m_buffers;
        Map<std::string, RHIUAVRef>    m_uavs;
        Map<std::string, RHISRVRef>    m_srvs;

        EntitySet m_entities;
        EntitySet m_cameras;
    };

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

    void Scene::ForEach(std::function<void(Entity)> func) const noexcept {
        m_impl->ForEach(std::move(func));
    }

    Scene* Scene::GetDefaultScene() noexcept {
        return g_scene;
    }
    void Scene::SetDefaultScene(Scene* scene) noexcept {
        g_scene = scene;
    }

}// namespace Moer