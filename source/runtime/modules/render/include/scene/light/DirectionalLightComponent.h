#pragma once

#include "math/Base.h"
#include "scene/light/LightComponent.h"

namespace Moer {

    class RENDER_API DirectionalLightComponent : public LightComponent {

    public:
        DirectionalLightComponent(Vector3f color, float intensity, Vector3f direction) noexcept
            : LightComponent(color, intensity), m_direction(direction) {}

        Vector3f GetDirection() const noexcept { return m_direction; }
        void     SetDirection(Vector3f direction) noexcept { m_direction = direction; }

    private:
        Vector3f m_direction;
    };

}// namespace Moer
