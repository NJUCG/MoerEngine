#pragma once
#include <unordered_set>
#include <vector>

#include "API_Macro.h"
#include "Entity.h"

namespace Moer {


class RENDER_API Scene {
    public:
        Scene() noexcept;

        void AddEntity(Entity entity) noexcept;
        void RemoveEntity(Entity entity) noexcept;
        Array<Entity> GetEntities() const noexcept;    
        
        static  Scene * GetDefaultScene() noexcept;
        static  void SetDefaultScene(Scene * scene) noexcept;

    protected:
        inline static Scene * m_default_scene = nullptr;
        std::unordered_set<Entity, Entity::Hasher> entities;
};

extern RENDER_API Scene * g_scene;
    
}