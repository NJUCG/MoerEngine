#ifndef MOER_AO_GEOMETRY_HLSLI
#define MOER_AO_GEOMETRY_HLSLI

#include "pipelines/RasterCommon.hlsli"

// Depth, normal and reconstruction ray must refer to the same full-resolution texel.
struct AoSurface {
    float2 uv;
    float depth;
    float3 normal;
};

AoSurface LoadAoSurface(float2 uv, uint depth_handle, uint normal_handle) {
    uint width, height;
    TextureHandle(depth_handle).GetTexture2D<float>().GetDimensions(width, height);
    uint2 size = uint2(width, height);
    uint2 pixel = min(uint2(saturate(uv) * size), size - 1);
    AoSurface surface;
    surface.uv = (float2(pixel) + 0.5) / size;
    surface.depth = TextureHandle(depth_handle).GetTexture2D<float>().Load(int3(pixel, 0));
    float3 normal = Raster::UnpackNormal(TextureHandle(normal_handle).GetTexture2D<float3>().Load(int3(pixel, 0)));
    surface.normal = dot(normal, normal) > Epsilon ? normalize(normal) : float3(0, 0, 0);
    return surface;
}

bool IsAoSurfaceValid(AoSurface surface) {
    return surface.depth > Epsilon && dot(surface.normal, surface.normal) > 0.5;
}

float3 SampleAoCosineHemisphere(float3 normal, float2 random_values) {
    float phi = 2.0 * PI * random_values.x;
    float cos_theta = sqrt(1.0 - random_values.y);
    float sin_theta = sqrt(random_values.y);
    float3 local_direction = float3(cos(phi) * sin_theta, sin(phi) * sin_theta, cos_theta);
    float3 up = abs(normal.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 tangent = normalize(cross(up, normal));
    float3 bitangent = cross(normal, tangent);
    return local_direction.x * tangent + local_direction.y * bitangent + local_direction.z * normal;
}

#endif
