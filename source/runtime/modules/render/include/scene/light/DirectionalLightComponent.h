#pragma once

#include "math/Base.h"
#include "scene/light/LightComponent.h"

namespace Moer {

    class RENDER_API DirectionalLightComponent : public LightComponent {

    public:
        DirectionalLightComponent() noexcept {}
        DirectionalLightComponent(Vector3f color, float intensity, Vector3f direction, float _angular_size) noexcept
            : LightComponent(color, intensity, ELightComponentType::DIRECTIONAL), m_direction(direction), angluar_size(_angular_size) {}

        Vector3f GetDirection() const noexcept { return m_direction; }
        void     SetDirection(Vector3f direction) noexcept { m_direction = direction; }
        //Todo Handle Position for Shadow Map
        LightComponentData ToData() const noexcept override {
            LightComponentData data;
            data.color     = GetColor();
            data.intensity = GetIntensity();
            data.position  = Vector3f(0.0f);
            data.direction = m_direction;
            data.info      = Vector4f(0.0f, 0.0f, 0.0f, 0.0f);
            data.type      = static_cast<uint32_t>(GetType());
            return data;
        }
        float angluar_size{0.f};

    private:
        Vector3f m_direction{};
    };

}// namespace Moer
