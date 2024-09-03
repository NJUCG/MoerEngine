#pragma once

#include "math/Base.h"
#include "scene/light/LightComponent.h"

namespace Moer {

    class RENDER_API SpotLightComponent : public LightComponent {

    public:
        SpotLightComponent(
            Vector3f color,
            float    intensity,
            Vector3f position,
            Vector3f direction,
            float    inner_cone_angle,
            float    outer_cone_angle) noexcept
            : LightComponent(color, intensity),
              m_position(position),
              m_direction(direction),
              m_inner_cone_angle(inner_cone_angle),
              m_outer_cone_angle(outer_cone_angle) {}

        Vector3f GetPosition() const noexcept { return m_position; }
        void     SetPosition(Vector3f position) noexcept { m_position = position; }
        Vector3f GetDirection() const noexcept { return m_direction; }
        void     SetDirection(Vector3f direction) noexcept { m_direction = direction; }
        float    GetInnerConeAngle() const noexcept { return m_inner_cone_angle; }
        void     SetInnerConeAngle(float inner_cone_angle) noexcept { m_inner_cone_angle = inner_cone_angle; }
        float    GetOuterConeAngle() const noexcept { return m_outer_cone_angle; }
        void     SetOuterConeAngle(float outer_cone_angle) noexcept { m_outer_cone_angle = outer_cone_angle; }

    private:
        Vector3f m_position;
        Vector3f m_direction;
        float    m_inner_cone_angle;
        float    m_outer_cone_angle;
    };

}// namespace Moer