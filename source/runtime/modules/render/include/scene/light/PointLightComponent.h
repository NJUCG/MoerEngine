#pragma once

#include "math/Base.h"
#include "scene/light/LightComponent.h"

namespace Moer {

    class RENDER_API PointLightComponent : public LightComponent {

    public:
        PointLightComponent() noexcept {}
        PointLightComponent(Vector3f color, float intensity, Vector3f position) noexcept
            : LightComponent(color, intensity, ELightComponentType::POINT), m_position(position) {}

        Vector3f GetPosition() const noexcept { return m_position; }
        void     SetPosition(Vector3f position) noexcept { m_position = position; }

    private:
        Vector3f m_position{};
    };

}// namespace Moer