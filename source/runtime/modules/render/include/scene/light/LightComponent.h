#pragma once

#include "misc/CountableRef.h"
#include "misc/STL.h"
#include "math/Base.h"
#include "RenderAPI.h"

#include <cstdint>

namespace Moer {

    // In shader, assume that only directional light has a strong ambient (in blinn phong model)
    enum class ELightComponentType : uint8_t {
        NONE = 0,
        DIRECTIONAL,
        POINT,
        SPOT,
    };

    /**
     * This class implements the light component in ECS.
     * This is the base class and should not be initialized directly.
     * I name this class xxxComponent to make it clear: it is a component in ECS.
     */
    class RENDER_API LightComponent : public CountableResource {

    public:
        LightComponent() noexcept
            : m_color(Vector3f(1.0f)), m_intensity(1.0f), m_type(ELightComponentType::NONE) {}
        LightComponent(Vector3f color, float intensity, ELightComponentType type) noexcept
            : m_color(color), m_intensity(intensity), m_type(type) {}

        virtual ~LightComponent() noexcept                              = default;
        LightComponent(const LightComponent& light) noexcept            = default;
        LightComponent& operator=(const LightComponent& light) noexcept = default;
        LightComponent(LightComponent&& light) noexcept                 = default;
        LightComponent& operator=(LightComponent&& light) noexcept      = default;

        Vector3f            GetColor() const noexcept { return m_color; }
        void                SetColor(Vector3f color) noexcept { m_color = color; }
        float               GetIntensity() const noexcept { return m_intensity; }
        void                SetIntensity(float intensity) noexcept { m_intensity = intensity; }
        ELightComponentType GetType() const noexcept { return m_type; }
        // m_type is not settable

        static Array<CountableRef<LightComponent>> CreateDefaultLightComponents();

    private:
        Vector3f            m_color;
        float               m_intensity;
        ELightComponentType m_type;// This field is used to distinguish type when read & write
    };

    using LightComponentRef = CountableRef<LightComponent>;

}// namespace Moer