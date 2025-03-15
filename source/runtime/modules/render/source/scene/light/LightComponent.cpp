#include "scene/light/LightComponent.h"

#include "misc/STL.h"
#include "log/LogSystem.h"

namespace Moer {

    void LightComponent::WriteToStream(OutputStream& _stream) const {
        _stream << GetType();
        _stream << GetColor();
        _stream << GetIntensity();

        if (GetType() == ELightComponentType::DIRECTIONAL) {
            auto* dir_light = dynamic_cast<const DirectionalLightComponent*>(this);
            _stream << dir_light->GetDirection();

        } else if (GetType() == ELightComponentType::POINT) {
            auto* point_light = dynamic_cast<const PointLightComponent*>(this);
            _stream << point_light->GetPosition();

        } else if (GetType() == ELightComponentType::SPOT) {
            auto* spot_light = dynamic_cast<const SpotLightComponent*>(this);
            _stream << spot_light->GetPosition();
            _stream << spot_light->GetDirection();
            _stream << spot_light->GetInnerConeAngle();
            _stream << spot_light->GetOuterConeAngle();

        } else if (GetType() == ELightComponentType::ENVIRONMENT) {
            auto* environment_light = dynamic_cast<const EnvironmentLightComponent*>(this);
            // _stream << environment_light->bdls_handle;
            _stream << environment_light->size;
            // _stream << environment_light->rotation;

        } else if (GetType() == ELightComponentType::AMBIENT) {
            // do nothing

        } else {
            LOG_WARNING("Unknown light type: {}", static_cast<uint8_t>(GetType()));
        }
    }

    CountableRef<LightComponent> LightComponent::ReadFromStream(InputStream& _stream) {
        ELightComponentType type;
        float3              color;
        float               intensity;

        _stream >> type >> color >> intensity;

        if (type == ELightComponentType::DIRECTIONAL) {
            float3 direction;
            _stream >> direction;

            return MoerNew(DirectionalLightComponent)(
                color,
                intensity,
                direction,
                0.f);

        } else if (type == ELightComponentType::POINT) {
            float3 position;
            _stream >> position;

            return MoerNew(PointLightComponent)(
                color,
                intensity,
                position);

        } else if (type == ELightComponentType::SPOT) {
            float3 position;
            float3 direction;
            float  inner_cone_angle;
            float  outer_cone_angle;
            _stream >> position >> direction >> inner_cone_angle >> outer_cone_angle;

            return MoerNew(SpotLightComponent)(
                color,
                intensity,
                position,
                direction,
                inner_cone_angle,
                outer_cone_angle);

        } else if (type == ELightComponentType::ENVIRONMENT) {
            uint2 size;
            _stream >> size;

            return MoerNew(EnvironmentLightComponent)(
                color,
                size);

        } else if (type == ELightComponentType::AMBIENT) {
            return MoerNew(AmbientLightComponent)(color);

        } else {
            LOG_ERROR("Unknown light type: {}. Code Error!", static_cast<uint8_t>(type));
            return nullptr;
        }
    }

    Array<LightComponentRef> LightComponent::CreateDefaultLightComponents() {
        Array<LightComponentRef> lights;

        // Directional light
        {
            LightComponentRef directional_light = MoerNew(DirectionalLightComponent)(
                Vector3f(1.0f, 1.0f, 1.0f),   // color
                2.0f,                         // intensity
                Vector3f(-1.0f, -1.0f, -1.0f),// direction
                0.f);
            lights.push_back(directional_light);
        }
        {
            LightComponentRef directional_light = MoerNew(DirectionalLightComponent)(
                Vector3f(1.0f, 1.0f, 1.0f), // color
                0.3f,                       // intensity
                Vector3f(1.0f, -2.0f, 1.0f),// direction
                0.f);
            lights.push_back(directional_light);
        }
        {
            LightComponentRef directional_light = MoerNew(DirectionalLightComponent)(
                Vector3f(1.0f, 1.0f, 1.0f), // color
                0.3f,                       // intensity
                Vector3f(0.0f, -1.0f, 0.0f),// direction
                0.f);
            lights.push_back(directional_light);
        }

        // Ambient Light
        {
            float ambient_intensity = 0.05f;
            lights.push_back(MoerNew(AmbientLightComponent)(
                Vector3f(ambient_intensity, ambient_intensity, ambient_intensity)));
        }

        // Point light
        {
            // The following lights should be placed in the Sponza scene.

            static auto randf = []() -> float {
                return static_cast<float>(rand()) / (RAND_MAX);
            };

            // center
            auto  light_pos_center = float3(-0.5f, 0.2f, -0.25f);
            auto  light_color      = float3(1.0f, 1.0f, 1.0f);
            float dz[2]            = {-1.0f, 1.0f};
            for (int i = -6; i < 6; ++i) {
                for (int j = 0; j < 2; ++j) {
                    auto pos = float3(
                        light_pos_center.x + i * 1.5f,
                        light_pos_center.y,
                        light_pos_center.z + dz[j]);

                    auto t = randf();
                    if (t < 0.15) {
                        light_color.x = 1.0f;
                        light_color.y = 0.0f;
                        light_color.z = 0.0f;

                    } else if (t < 0.3) {
                        light_color.x = 0.0f;
                        light_color.y = 1.0f;
                        light_color.z = 0.0f;

                    } else if (t < 0.45) {
                        light_color.x = 0.0f;
                        light_color.y = 0.0f;
                        light_color.z = 1.0f;

                    } else if (t < 0.6) {
                        light_color.x = 0.0f;
                        light_color.y = 1.0f;
                        light_color.z = 1.0f;

                    } else if (t < 0.75) {
                        light_color.x = 1.0f;
                        light_color.y = 0.0f;
                        light_color.z = 1.0f;

                    } else if (t < 0.9) {
                        light_color.x = 1.0f;
                        light_color.y = 1.0f;
                        light_color.z = 0.0f;

                    } else {
                        light_color.x = 1.0f;
                        light_color.y = 1.0f;
                        light_color.z = 1.0f;
                    }

                    LightComponentRef point_light = MoerNew(PointLightComponent)(light_color, 1.0f, pos);

                    lights.push_back(point_light);
                }
            }
        }

        return lights;
    }

}// namespace Moer