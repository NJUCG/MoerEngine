#ifndef FRAMEWORK_LIGHTING_HLSL
#define FRAMEWORK_LIGHTING_HLSL

// #define Directional_LIGHT_TYPE 1
// #define Point_LIGHT_TYPE 2
// #define Spot_LIGHT_TYPE 3

struct  Light
{
    // color.w represents light intensity
    float4 color;
    // position.w represents type of light
    float4 position;
    float4 direction;
    // (only used for spot lights) info.x represents light inner cone angle, info.y represents light outer cone angle
    float4 info;
};

float3 apply_directional_light(Light light, float3 normal)
{
    float3 world_to_light = -light.direction.xyz;
    world_to_light      = normalize(world_to_light);
    float ndotl         = clamp(dot(normal, world_to_light), 0.0, 1.0);
    return ndotl * light.color.w * light.color.rgb;
}

float3 apply_point_light(Light light, float3 pos, float3 normal)
{
    
    float3  world_to_light = light.position.xyz - pos;
    float dist           = length(world_to_light) * 0.005f;
    float atten          = 1.0 / (dist * dist);
    world_to_light       = normalize(world_to_light);
    float ndotl          = clamp((dot(normal, world_to_light)), 0.0, 1.0);
    return ndotl * light.color.w * atten * light.color.rgb;
}

float3 apply_spot_light(Light light, float3 pos, float3 normal)
{
    float3  light_to_pixel   = normalize(pos - light.position.xyz);
    float theta            = dot(light_to_pixel, normalize(light.direction.xyz));
    float inner_cone_angle = light.info.x;
    float outer_cone_angle = light.info.y;
    float intensity        = (theta - outer_cone_angle) / (inner_cone_angle - outer_cone_angle);
    return intensity * light.color.w * light.color.rgb;
}

float3 apply_light(Light light, float3 pos, float3 normal)
{
    if (light.direction.w == 1)
    {
        return apply_directional_light(light, normal);
    }
    else if (light.direction.w == 2)
    {
        return apply_point_light(light, pos, normal);
    }
    else if (light.direction.w == 3){
        return apply_point_light(light, pos, normal);
    }
    return float3(0,0,0);
}

#endif 