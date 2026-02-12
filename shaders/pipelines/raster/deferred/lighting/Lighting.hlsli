#ifndef RASTER_LIGHTING_LIGHTING_HLSLI
#define RASTER_LIGHTING_LIGHTING_HLSLI

#include "materials/Brdf.hlsli"
#include "shared/scene/SharedSceneStruct.h"
#include "shared/raster/SharedEnum.h"

struct LightContext {
    BRDFContext brdf_context;
    float3      frag_pos;
    uint        lut_ggx_emu_handle;

    Moer::GLight light;

    float3 accumulated_color;

    void Init(BRDFContext _brdf_context, float3 _frag_pos, uint _lut_ggx_emu_handle) {
        brdf_context       = _brdf_context;
        frag_pos           = _frag_pos;
        lut_ggx_emu_handle = _lut_ggx_emu_handle;

        accumulated_color = float3(0.0f, 0.0f, 0.0f);
    }

    void AccumulateLight(Moer::GLight _light, float shadow) {
        light = _light;

        if (light.type == Moer::ELightType::Directional) {
            accumulated_color += _AccumulateDirectionalLight(shadow);
        } else if (light.type == Moer::ELightType::Point) {
            accumulated_color += _AccumulatePointLight();
        } else if (light.type == Moer::ELightType::Spot) {
            accumulated_color += _AccumulateSpotLight();
        } else if (light.type == Moer::ELightType::Environment) {
            accumulated_color += _AccumulateEnvironmentLight();
        } else if (light.type == Moer::ELightType::Ambient) {
            accumulated_color += _AccumulateAmbientLight();
        }
    }

    float3 GetResult() {
        return accumulated_color;
    }

    float3 _AccumulateDirectionalLight(float shadow) {
        float3 L   = normalize(-light.direction.xyz);
        float  NoL = saturate(dot(brdf_context.N, L));

        brdf_context.UpdatePerLight(
            light.intensity * light.color.rgb,
            L,
            TextureHandle(lut_ggx_emu_handle).Sample2D<float3>(float2(NoL, brdf_context.roughness))
        );

        return brdf_context.Evaluate() * shadow;
    }

    float3 _AccumulatePointLight() {
        float3 L_unnorm = light.position.xyz - frag_pos;

        float3 L   = normalize(L_unnorm);
        float  NoL = saturate(dot(brdf_context.N, L));

        brdf_context.UpdatePerLight(
            light.intensity * light.color.rgb,
            L,
            TextureHandle(lut_ggx_emu_handle).Sample2D<float3>(float2(NoL, brdf_context.roughness))
        );

        float dist2       = dot(L_unnorm, L_unnorm);
        float attenuation = 1.0 / (4.0 * PI * dist2); // Irradiance = Intensity / (4πr²) * NoL = attenuation * NoL

        return brdf_context.Evaluate() * attenuation;
    }

    float3 _AccumulateSpotLight() {
        return light.intensity * light.color.rgb;
    }
    // float3 apply_spot_light(LightData light, float3 frag_pos, float3 normal)
    // {
    //     float3  light_to_pixel   = normalize(frag_pos - light.position.xyz);
    //     float theta            = dot(light_to_pixel, normalize(light.direction.xyz));
    //     float inner_cone_angle = light.info.x;
    //     float outer_cone_angle = light.info.y;
    //     float intensity        = (theta - outer_cone_angle) / (inner_cone_angle - outer_cone_angle);
    //     return intensity * light.intensity * light.color.rgb;
    // }

    float3 _AccumulateEnvironmentLight() {
        return light.intensity * light.color.rgb;
    }

    float3 _AccumulateAmbientLight() {
        return light.intensity * light.color.rgb;
    }
};

#endif