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
        void               SetPosition(Vector3f position) noexcept { m_position = position; }
        LightComponentData ToData() const noexcept override {
            LightComponentData data;
            data.color      = GetColor();
            data.intensity  = GetIntensity();
            data.position   = m_position;
            data.direction  = Vector3f(0.0f);
            data.info       = Vector4f(0.0f, 0.0f, 0.0f, 0.0f);
            data.type       = static_cast<uint32_t>(GetType());
            return data;
        }

    private:
        Vector3f m_position{};
    };

}// namespace Moer