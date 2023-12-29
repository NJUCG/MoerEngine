#include "scene/Scene.h"

#include "config/ConfigManager.h"
// #include "loader/gltf/Parser.h"
#include "scene/EntityManager.h"
#include "scene/RenderableManager.h"
#include "rhi/RHI.h"

namespace Moer {
    // Scene * Scene::default_scene = nullptr;
    Scene* g_scene = nullptr;

    class RENDER_API Scene::Impl {
    public:
        void          AddEntity(Entity entity) noexcept { m_entities.emplace(entity); }
        void          AddCamera(Entity entity) noexcept { m_cameras.emplace(entity); }
        void          RemoveEntity(Entity entity) noexcept { m_entities.erase(entity); };
        Array<Entity> GetEntities() const noexcept;
        Array<Entity> GetCameras() const noexcept;

    protected:
        EntitySet m_entities;
        EntitySet m_cameras;
    };

    Array<Entity> Scene::Impl::GetEntities() const noexcept {
        Array<Entity> result;
        result.reserve(m_entities.size());
        for (auto& entity : m_entities) {
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

    void Scene::AddCamera(Entity entity) noexcept {
        m_impl->AddCamera(entity);
    }

    Array<Entity> Scene::GetEntities() const noexcept {
        return m_impl->GetEntities();
    }

    Array<Entity> Scene::GetCameras() const noexcept {
        return m_impl->GetCameras();
    }

    Scene* Scene::GetDefaultScene() noexcept {
        return g_scene;
    }
    void Scene::SetDefaultScene(Scene* scene) noexcept {
        g_scene = scene;
    }

}