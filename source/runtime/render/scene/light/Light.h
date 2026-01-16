#pragma once

#include "RenderAPI.h"
#include "math/Base.h"
#include "misc/CountableRef.h"
#include "misc/STL.h"
#include "misc/Traits.h"
#include "serialize/Serializer.h"

namespace Moer {

enum class ELightComponentType : uint8_t {
    NONE = 0,
    DIRECTIONAL,
    POINT,
    SPOT,
    ENVIRONMENT,
    AMBIENT,
};

struct LightComponentData {
    Vector3f color;
    float    intensity;
    uint32_t type;
    Vector3f position;
    Vector3f direction;
    Vector4f info;
};

/**
     * This class implements the light component in ECS.
     * This is the base class and should not be initialized directly.
     * I name this class xxxComponent to make it clear: it is a component in ECS.
     */
class RENDER_API LightComponent : public CountableResource {

public:
    LightComponent() noexcept :
        m_color(Vector3f(1.0f)),
        m_intensity(1.0f),
        m_type(ELightComponentType::NONE) {}
    LightComponent(Vector3f _color, float _intensity, ELightComponentType _type) noexcept :
        m_color(_color),
        m_intensity(_intensity),
        m_type(_type) {}

    virtual ~LightComponent() noexcept = default;
    // LightComponent(const LightComponent& _light) noexcept           = delete;
    // LightComponent& operator=(const LightComponent& light) noexcept = delete;
    // LightComponent(LightComponent&& _light) noexcept                = delete;
    // LightComponent& operator=(LightComponent&& light) noexcept      = default;

    Vector3f GetColor() const noexcept {
        return m_color;
    }
    void SetColor(Vector3f _color) noexcept {
        m_color = _color;
    }
    float GetIntensity() const noexcept {
        return m_intensity;
    }
    void SetIntensity(float _intensity) noexcept {
        m_intensity = _intensity;
    }
    ELightComponentType GetType() const noexcept {
        return m_type;
    }
    virtual LightComponentData ToData() const noexcept = 0;
    // m_type is not settable

    static Array<CountableRef<LightComponent>> CreateDefaultLightComponents();

    void WriteToStream(OutputStream& _stream) const;

    static CountableRef<LightComponent> ReadFromStream(InputStream& _stream);

private:
    Vector3f            m_color;
    float               m_intensity;
    ELightComponentType m_type; // This field is used to distinguish type when read & write
};

using LightComponentRef = CountableRef<LightComponent>;

// MARK: Directional
class RENDER_API DirectionalLightComponent : public LightComponent {
public:
    DirectionalLightComponent() noexcept {}
    DirectionalLightComponent(
        Vector3f color,
        float    intensity,
        Vector3f direction,
        float    angular_size
    ) noexcept :
        LightComponent(color, intensity, ELightComponentType::DIRECTIONAL),
        m_direction(direction),
        m_angluar_size(angular_size) {}

    Vector3f GetDirection() const noexcept {
        return m_direction;
    }
    void SetDirection(Vector3f direction) noexcept {
        m_direction = direction;
    }
    float GetAngularSize() const noexcept {
        return m_angluar_size;
    }
    void SetAngularSize(float angular_size) noexcept {
        m_angluar_size = angular_size;
    }
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

private:
    Vector3f m_direction{};
    float    m_angluar_size{0.f};
};

// MARK: Point
class RENDER_API PointLightComponent : public LightComponent {

public:
    PointLightComponent() noexcept {}
    PointLightComponent(Vector3f color, float intensity, Vector3f position, float radius = 100.0f) noexcept :
        LightComponent(color, intensity, ELightComponentType::POINT),
        m_position(position),
        m_radius(radius) {}

    Vector3f GetPosition() const noexcept {
        return m_position;
    }
    void SetPosition(Vector3f position) noexcept {
        m_position = position;
    }

    float GetRadius() const noexcept {
        return m_radius;
    }
    void SetRadius(float radius) noexcept {
        m_radius = radius;
    }

    LightComponentData ToData() const noexcept override {
        LightComponentData data;
        data.color     = GetColor();
        data.intensity = GetIntensity();
        data.position  = m_position;
        data.direction = Vector3f(0.0f);
        data.info      = Vector4f(m_radius, 0.0f, 0.0f, 0.0f); // 将半径存储在 info.x 中
        data.type      = static_cast<uint32_t>(GetType());
        return data;
    }

private:
    Vector3f m_position{};
    float    m_radius{10.0f};
};

// MARK: Spot
class RENDER_API SpotLightComponent : public LightComponent {
public:
    SpotLightComponent() noexcept {}
    SpotLightComponent(
        Vector3f color,
        float    intensity,
        Vector3f position,
        Vector3f direction,
        float    inner_cone_angle,
        float    outer_cone_angle
    ) noexcept :
        LightComponent(color, intensity, ELightComponentType::SPOT),
        m_position(position),
        m_direction(direction),
        m_inner_cone_angle(inner_cone_angle),
        m_outer_cone_angle(outer_cone_angle) {}

    Vector3f GetPosition() const noexcept {
        return m_position;
    }
    void SetPosition(Vector3f position) noexcept {
        m_position = position;
    }
    Vector3f GetDirection() const noexcept {
        return m_direction;
    }
    void SetDirection(Vector3f direction) noexcept {
        m_direction = direction;
    }
    float GetInnerConeAngle() const noexcept {
        return m_inner_cone_angle;
    }
    void SetInnerConeAngle(float inner_cone_angle) noexcept {
        m_inner_cone_angle = inner_cone_angle;
    }
    float GetOuterConeAngle() const noexcept {
        return m_outer_cone_angle;
    }
    void SetOuterConeAngle(float outer_cone_angle) noexcept {
        m_outer_cone_angle = outer_cone_angle;
    }
    LightComponentData ToData() const noexcept override {
        LightComponentData data;
        data.color     = GetColor();
        data.intensity = GetIntensity();
        data.position  = m_position;
        data.direction = m_direction;
        data.info      = Vector4f(m_inner_cone_angle, m_outer_cone_angle, 0.0f, 0.0f);
        data.type      = static_cast<uint32_t>(GetType());
        return data;
    }

private:
    Vector3f m_position{};
    Vector3f m_direction{};
    float    m_inner_cone_angle = 0.0f;
    float    m_outer_cone_angle = 0.0f;
};

// MARK: Environment
class RENDER_API EnvironmentLightComponent : public LightComponent {
public:
    EnvironmentLightComponent() noexcept :
        LightComponent(Vector3f(1.0f), 1.0f, ELightComponentType::ENVIRONMENT),
        rotation(0.f) {}

    EnvironmentLightComponent(float3 _scale, uint2 _size) noexcept :
        LightComponent(_scale, 1.0f, ELightComponentType::ENVIRONMENT),
        size(_size),
        rotation(0.f) {}

    // TODO: use getter and setter (or delete all other light components getter and setter)

    float3 GetColorScale() const noexcept {
        return GetColor();
    }
    virtual LightComponentData ToData() const noexcept override {
        LightComponentData data;
        data.color     = GetColor();
        data.intensity = GetIntensity();
        data.position  = Vector3f(0.0f);
        data.direction = Vector3f(0.0f);
        data.info      = Vector4f(bdls_handle, rotation, size.x, size.y);
        data.type      = static_cast<uint32_t>(GetType());
        return data;
    }
    uint  bdls_handle;
    uint2 size;
    float rotation;
};

// MARK: Ambienf
class RENDER_API AmbientLightComponent : public LightComponent {
public:
    AmbientLightComponent() noexcept {}
    AmbientLightComponent(Vector3f color) noexcept :
        LightComponent(color, 0.f, ELightComponentType::AMBIENT) {}

    LightComponentData ToData() const noexcept override {
        LightComponentData data;
        data.color     = GetColor();
        data.intensity = 0.0f;
        data.position  = Vector3f(0.0f);
        data.direction = Vector3f(0.0f);
        data.info      = Vector4f(0.0f, 0.0f, 0.0f, 0.0f);
        data.type      = static_cast<uint32_t>(GetType());
        return data;
    }
};

} // namespace Moer