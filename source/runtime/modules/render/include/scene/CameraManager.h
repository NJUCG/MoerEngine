#pragma once
#include "Camera.h"
#include "ECS.h"
#include "Entity.h"
#include "misc/CountableRef.h"

namespace Moer {
    
    using CameraRef = CountableRef<Camera>;

    class  RENDER_API CameraManager {
        struct Proxy {
            CameraRef camera;
        };
        EntityComponentManger<Proxy> m_manager;
    public:
        CameraRef Get(Entity entity) noexcept;
        CameraRef Create(Entity entity) noexcept;
        bool HasComponent(Entity entity) const noexcept;
        void Destroy(Entity entity) noexcept;

        static CameraManager & Get() noexcept;
    protected:
        inline  static std::unique_ptr<CameraManager> m_instance = nullptr;
    };
}
