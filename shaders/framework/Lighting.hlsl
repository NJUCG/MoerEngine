#ifndef FRAMEWORK_LIGHTING_HLSL
#define FRAMEWORK_LIGHTING_HLSL

// #define Directional_LIGHT_TYPE 1
// #define Point_LIGHT_TYPE 2
// #define Spot_LIGHT_TYPE 3
static const uint Directional_LIGHT_TYPE = 1;
static const uint Point_LIGHT_TYPE = 2;
static const uint Spot_LIGHT_TYPE = 3;

struct LightData{
    float3 color;
    float intensity;
    uint type;

    float3 position;
    float3 direction;
    float4 info;

};

float3 apply_directional_light(LightData light, float3 normal)
{
    float3 world_to_light = -light.direction.xyz;
    world_to_light      = normalize(world_to_light);
    float ndotl         = clamp(dot(normal, world_to_light), 0.0, 1.0);
    return ndotl * light.intensity * light.color.rgb;
}

float3 apply_point_light(LightData light, float3 pos, float3 normal)
{
    
    float3  world_to_light = light.position.xyz - pos;
    float dist           = length(world_to_light) * 0.005f;
    float atten          = 1.0 / (dist * dist);
    world_to_light       = normalize(world_to_light);
    float ndotl          = clamp((dot(normal, world_to_light)), 0.0, 1.0);
    return ndotl * light.intensity * atten * light.color.rgb;
}

float3 apply_spot_light(LightData light, float3 pos, float3 normal)
{
    float3  light_to_pixel   = normalize(pos - light.position.xyz);
    float theta            = dot(light_to_pixel, normalize(light.direction.xyz));
    float inner_cone_angle = light.info.x;
    float outer_cone_angle = light.info.y;
    float intensity        = (theta - outer_cone_angle) / (inner_cone_angle - outer_cone_angle);
    return intensity * light.intensity * light.color.rgb;
}

float3 apply_light(LightData light, float3 pos, float3 normal)
{
    int light_type = light.type;
    if (light_type == Directional_LIGHT_TYPE)
    {
        return apply_directional_light(light, normal);
    }
    else if (light_type == Point_LIGHT_TYPE)
    {
        return apply_point_light(light, pos, normal);
    }
    else if (light_type == Spot_LIGHT_TYPE)
    {
        return apply_spot_light(light, pos, normal);
    }
    return float3(0.0f, 0.0f, 0.0f);
}

float3 calculate_light_dir(LightData light, float3 pos)
{
    if (light.type == Directional_LIGHT_TYPE)
    {
        return -light.direction.xyz;
    }
    else
    {
        return light.position.xyz - pos;
    }
}

#define DISTANCE_ATTENUATION_FACTOR 0.006f
#define DIRECTIONAL_LIGHT_AMBIENT_FACTOR 0.3f
#define POINT_LIGHT_AMBIENT_FACTOR 0.1f
#define SPECULAR_EXPONENT 64.0f

// float3 apply_directional_light_blinn_phong(Light light, float3 frag_pos, float3 normal, float3 kd, float3 ks, float3 camera_position) {
//     float3 frag_to_light = normalize(-light.direction.xyz);
//     float3 frag_to_camera = normalize(camera_position - frag_pos);
//     float3 h = normalize(frag_to_camera + frag_to_light);
//     // ambient
//     float3 ka = DIRECTIONAL_LIGHT_AMBIENT_FACTOR * kd;
//     float3 ambient = ka * light.color.rgb;
//     // diffuse
//     float3 diffuse = kd * clamp(dot(normal, frag_to_light), 0, 1) * light.color.rgb;
//     // specular
//     float3 specular = ks * pow(clamp(dot(normal, h), 0, 1), SPECULAR_EXPONENT) * light.color.rgb;
//     // result
//     float3 result = ambient + diffuse + specular;
//     return result * light.color.w;
// }
//
// float3 apply_point_light_blinn_phong(Light light, float3 frag_pos, float3 normal, float3 kd, float3 ks, float3 camera_position) {
//     float3 frag_to_light = normalize(light.position.xyz - frag_pos);
//     float3 frag_to_camera = normalize(camera_position - frag_pos);
//     float3 h = normalize(frag_to_camera + frag_to_light);
//     float distance = length(light.position.xyz - frag_pos) * DISTANCE_ATTENUATION_FACTOR;
//     // ambient
//     float3 ka = POINT_LIGHT_AMBIENT_FACTOR * kd;
//     float3 ambient = ka * light.color.rgb;
//     // diffuse
//     float3 diffuse = kd * clamp(dot(normal, frag_to_light), 0, 1) * light.color.rgb;
//     // specular
//     float3 specular = ks * pow(clamp(dot(normal, h), 0, 1), SPECULAR_EXPONENT) * light.color.rgb;
//     // result
//     float attenuation = 1.0f / (distance * distance);
//     float3 result = ambient + diffuse + specular;
//     return result * light.color.w * attenuation;
// }
//
// float3 apply_light_blinn_phong(Light light, float3 frag_pos, float3 normal, float3 kd, float3 ks, float3 camera_position) {
//     float3 result = float3(0.0f, 0.0f, 0.0f);
//
//     // directional light need to be tested
//     if (light.direction.w == 1) {
//         result += apply_directional_light_blinn_phong(light, frag_pos, normal, kd, ks, camera_position);
//     } else if (light.direction.w == 2) {
//         result += apply_point_light_blinn_phong(light, frag_pos, normal, kd, ks, camera_position);
//     } else if (light.direction.w == 3) {
//         result += apply_point_light_blinn_phong(light, frag_pos, normal, kd, ks, camera_position);
//     }
//
//     return result;
// }


#endif 