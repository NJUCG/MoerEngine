#pragma once
#include "scene/ECS.h"
#include "Entity.h"
#include "math/Transform.h"

namespace Moer {
    using Transform = Moer::Transform;

    class RENDER_API TransformManager {
    public:
        const Transform&         Get(Entity entity) noexcept;
        Transform&               Create(Entity entity) noexcept;
        void                     Set(Entity entity, Transform transform) noexcept;
        bool                     HasComponent(Entity entity) const noexcept;
        void                     Destroy(Entity entity) noexcept;
        static TransformManager& Get() noexcept;

    protected:
        struct Proxy {
            Transform transform;
        };
        EntityComponentManger<Proxy> m_manager;
    };
}// namespace Moer