#ifndef MOER_GAUSSIAN_SHADOW_SAMPLING
#define MOER_GAUSSIAN_SHADOW_SAMPLING
#include "shared/gaussian/GaussianShadowParameters.h"
// Raw-buffer matrix loads do not preserve the typed-buffer row-major interpretation.
// Read explicit rows so Mesh and GS consumers use the same CPU matrix layout.
Moer::GaussianShadowData LoadGaussianShadowData(uint handle) {
    ArrayBuffer buffer = ArrayBuffer(handle);
    Moer::GaussianShadowData data = buffer.Load<Moer::GaussianShadowData>(0);
    [unroll] for (uint view = 0; view < 6; ++view)
        [unroll] for (uint row = 0; row < 4; ++row)
            data.world_to_clip[view][row] = buffer.Load<float4>(view * 4 + row);
    [unroll] for (uint row = 0; row < 4; ++row)
        data.world_to_view[row] = buffer.Load<float4>(24 + row);
    return data;
}
float GaussianShadowCascade(Moer::GaussianShadowData data, uint handle, uint cascade, float3 world) {
    float4 clip = mul(data.world_to_clip[cascade], float4(world, 1));
    if (clip.w <= 0) return 1;
    float3 p = clip.xyz / clip.w;
    float2 uv = float2(p.x * .5 + .5, .5 - p.y * .5);
    if (any(uv < 0) || any(uv > 1) || p.z < 0 || p.z > 1) return 1;
    float visibility = 0;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x) {
            float2 sample_uv = uv + float2(x,y) / data.config.z;
            if (any(sample_uv < 0) || any(sample_uv > 1)) { visibility += 1; continue; }
            float depth = TextureHandle(handle).SampleLevel<float>(sample_uv,0).r;
            visibility += depth <= p.z + max(1e-8,abs(p.z)*1e-6) ? 1 : 0;
        }
    return visibility / 9;
}
float GaussianShadowVisibility(Moer::GaussianShadowData data, uint4 maps, float3 world, bool mesh_shadow = false) {
    if (data.config.x == 0) return 1;
    float3 toward_light = data.config.x == 1 ? data.light.xyz : data.light.xyz - world;
    float length_squared = dot(toward_light, toward_light);
    if (length_squared < 1e-12) return 1;
    world += toward_light * rsqrt(length_squared) * data.options.z;
    if (data.config.x == 1) {
        float depth = -mul(data.world_to_view, float4(world,1)).z;
        float ratio = (depth - data.clips.z) / (data.clips.w - data.clips.z);
        uint cascade = 0;
        while (cascade < data.config.y && ratio > data.splits[cascade]) ++cascade;
        if (cascade >= data.config.y || ratio < 0) return 1;
        float v = GaussianShadowCascade(data, maps[cascade], cascade, world);
        if (data.options.w != 0 && cascade + 1 < data.config.y && ratio > data.blends[cascade]) {
            float t = saturate((ratio-data.blends[cascade]) / max(1e-8,data.splits[cascade]-data.blends[cascade]));
            v = lerp(v, GaussianShadowCascade(data,maps[cascade+1],cascade+1,world),t);
        }
        return v;
    }
    float3 ray = world - data.light.xyz;
    float z = max(abs(ray.x),max(abs(ray.y),abs(ray.z)));
    if (z < data.clips.x || z > data.clips.y) return 1;
    float fragment_depth = data.clips.x / (data.clips.x-data.clips.y) -
        data.clips.x*data.clips.y / ((data.clips.x-data.clips.y)*z);
    float3 direction = normalize(ray);
    float3 tangent = normalize(cross(abs(direction.y) < .99 ? float3(0,1,0) : float3(1,0,0),direction));
    float3 bitangent = cross(direction,tangent);
    float visibility = 0;
    [unroll] for (int y=-1;y<=1;++y)
        [unroll] for (int x=-1;x<=1;++x) {
            float3 sample_dir = normalize(direction + (x*tangent+y*bitangent)*(2.0/data.config.z));
            // Adapt only the GS receiver to legacy Mesh face orientation. Each PCF tap
            // chooses its face before the flip, including taps crossing a cube seam.
            if(mesh_shadow && data.light_indices.z != 0) {
                float3 a = abs(sample_dir);
                if(a.x >= a.y && a.x >= a.z) {sample_dir.y = -sample_dir.y;sample_dir.x *= 1.000001;}
                else if(a.y >= a.z) {sample_dir.z = -sample_dir.z;sample_dir.y *= 1.000001;}
                else {sample_dir.y = -sample_dir.y;sample_dir.z *= 1.000001;}
            }
            float d = TextureHandle(maps.x).SampleLevelCube<float>(sample_dir,0).r;
            visibility += d <= fragment_depth + max(1e-8,abs(fragment_depth)*1e-6) ? 1 : 0;
        }
    return visibility/9;
}
#endif
