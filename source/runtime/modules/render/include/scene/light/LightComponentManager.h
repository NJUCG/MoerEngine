#pragma once

#include "scene/light/LightComponent.h"
#include "scene/ECS.h"
#include "scene/Entity.h"
#include "RenderAPI.h"

namespace Moer {

    class RENDER_API LightComponentManager {
    private:
        EntityComponentManger<LightComponent> m_manager;

    public:
        LightComponent& Get(Entity entity) noexcept;
        // LightComponent& Create(Entity entity) noexcept; // Cannot create a base light component
        void Put(Entity entity, const LightComponent& light) noexcept;
        bool HasComponent(Entity entity) const noexcept;
        void Destroy(Entity entity) noexcept;

        static LightComponentManager& Get() noexcept;
    };
}// namespace Moer