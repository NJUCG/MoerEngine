#pragma once
#include <unordered_set>
#include <vector>

#include "API_Macro.h"
#include "Entity.h"

namespace Moer {

    using EntitySet = std::unordered_set<Entity, Entity::Hasher>;

    class RENDER_API Scene {
    public:
        Scene() noexcept;
        ~Scene() noexcept;
        void          AddEntity(Entity entity) noexcept;
        void          AddCamera(Entity entity) noexcept;
        void          RemoveEntity(Entity entity) noexcept;
        Array<Entity> GetEntities() const noexcept;
        Array<Entity> GetCameras() const noexcept;

        static Scene* GetDefaultScene() noexcept;
        static void   SetDefaultScene(Scene* scene) noexcept;

    protected:
        inline static Scene* m_default_scene = nullptr;
        class Impl;
        Impl* m_impl = nullptr;
    };

    extern RENDER_API Scene* g_scene;

}