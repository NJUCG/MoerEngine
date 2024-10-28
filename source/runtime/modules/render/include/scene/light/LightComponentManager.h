#pragma once

#include "scene/light/LightComponent.h"
#include "scene/ECS.h"
#include "scene/Entity.h"
#include "RenderAPI.h"

namespace Moer {

    class RENDER_API LightComponentManager {
    private:
        EntityComponentManger<LightComponentRef> m_manager;

    public:
        LightComponentRef Get(Entity _entity) noexcept;
        // LightComponent& Create(Entity entity) noexcept; // Cannot create a base light component
        void Put(Entity _entity, LightComponentRef _light) noexcept;
        bool HasComponent(Entity _entity) const noexcept;
        void Destroy(Entity _entity) noexcept;

        static LightComponentManager& Get() noexcept;
    };
}// namespace Moer