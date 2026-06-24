#ifndef MOER_RASTER_DEFERRED_LIGHTING_PROBE_GI_HLSLI
#define MOER_RASTER_DEFERRED_LIGHTING_PROBE_GI_HLSLI

#include "shared/raster/ShaderParameters.h"

bool ProbeGIIsEnabled(Moer::LightingData lighting_data) {
    return lighting_data.probe_volume_config.x != 0 && lighting_data.probe_volume_config.z != 0 &&
           lighting_data.probe_volume_counts.w > 0;
}

uint3 ProbeGIGetCounts(Moer::LightingData lighting_data) {
    return uint3(
        max(lighting_data.probe_volume_counts.x, 1u),
        max(lighting_data.probe_volume_counts.y, 1u),
        max(lighting_data.probe_volume_counts.z, 1u)
    );
}

uint ProbeGIGetProbeIndex(uint3 coord, uint3 counts) {
    return coord.x + coord.y * counts.x + coord.z * counts.x * counts.y;
}

float3 ProbeGILoadIrradiance(Moer::LightingData lighting_data, uint probe_index) {
    ArrayBuffer probe_buffer = ArrayBuffer(lighting_data.probe_volume_config.z);
    Moer::ProbeGridProbeData probe = probe_buffer.Load<Moer::ProbeGridProbeData>(probe_index);
    return probe.irradiance.rgb * probe.irradiance.a;
}

float3 ProbeGIGetLocalCoord(Moer::LightingData lighting_data, float3 world_pos) {
    float3 spacing = max(lighting_data.probe_volume_spacing.xyz, float3(1e-4, 1e-4, 1e-4));
    return (world_pos - lighting_data.probe_volume_origin.xyz) / spacing;
}

bool ProbeGIIsInsideVolume(Moer::LightingData lighting_data, float3 world_pos) {
    float3 volume_min = lighting_data.probe_volume_origin.xyz;
    float3 volume_max = volume_min + lighting_data.probe_volume_extent.xyz;
    return all(world_pos >= volume_min) && all(world_pos <= volume_max);
}

float3 ProbeGISampleIrradiance(Moer::LightingData lighting_data, float3 world_pos, float3 normal) {
    if (!ProbeGIIsEnabled(lighting_data)) {
        return float3(0.0, 0.0, 0.0);
    }

    float3 biased_pos = world_pos + normal * lighting_data.probe_volume_origin.w;
    if (!ProbeGIIsInsideVolume(lighting_data, biased_pos)) {
        return float3(0.0, 0.0, 0.0);
    }

    uint3 counts = ProbeGIGetCounts(lighting_data);
    uint3 max_coord = counts - uint3(1, 1, 1);
    float3 max_coord_f = float3(max_coord.x, max_coord.y, max_coord.z);
    float3 local = clamp(ProbeGIGetLocalCoord(lighting_data, biased_pos), float3(0.0, 0.0, 0.0), max_coord_f);

    uint3 base_coord = uint3(floor(local));
    uint3 next_coord = min(base_coord + uint3(1, 1, 1), max_coord);
    float3 weight = frac(local);

    float3 c000 = ProbeGILoadIrradiance(lighting_data, ProbeGIGetProbeIndex(base_coord, counts));
    float3 c100 = ProbeGILoadIrradiance(lighting_data, ProbeGIGetProbeIndex(uint3(next_coord.x, base_coord.y, base_coord.z), counts));
    float3 c010 = ProbeGILoadIrradiance(lighting_data, ProbeGIGetProbeIndex(uint3(base_coord.x, next_coord.y, base_coord.z), counts));
    float3 c110 = ProbeGILoadIrradiance(lighting_data, ProbeGIGetProbeIndex(uint3(next_coord.x, next_coord.y, base_coord.z), counts));
    float3 c001 = ProbeGILoadIrradiance(lighting_data, ProbeGIGetProbeIndex(uint3(base_coord.x, base_coord.y, next_coord.z), counts));
    float3 c101 = ProbeGILoadIrradiance(lighting_data, ProbeGIGetProbeIndex(uint3(next_coord.x, base_coord.y, next_coord.z), counts));
    float3 c011 = ProbeGILoadIrradiance(lighting_data, ProbeGIGetProbeIndex(uint3(base_coord.x, next_coord.y, next_coord.z), counts));
    float3 c111 = ProbeGILoadIrradiance(lighting_data, ProbeGIGetProbeIndex(next_coord, counts));

    float3 c00 = lerp(c000, c100, weight.x);
    float3 c10 = lerp(c010, c110, weight.x);
    float3 c01 = lerp(c001, c101, weight.x);
    float3 c11 = lerp(c011, c111, weight.x);
    float3 c0 = lerp(c00, c10, weight.y);
    float3 c1 = lerp(c01, c11, weight.y);
    return lerp(c0, c1, weight.z);
}

float3 ProbeGIEvaluateDiffuse(
    Moer::LightingData lighting_data,
    float3 world_pos,
    float3 normal,
    float3 albedo,
    float metallic,
    float direct_shadow
) {
    float3 irradiance = ProbeGISampleIrradiance(lighting_data, world_pos, normal);
    float diffuse_weight = 0.55 + 0.45 * (1.0 - saturate(metallic));
    float normal_weight = 0.55 + 0.45 * saturate(normal.y * 0.5 + 0.5);
    float shadow_visibility = lerp(1.25, 0.18, saturate(direct_shadow));
    float3 surface_tint = lerp(float3(1.0, 1.0, 1.0), albedo, 0.70);
    return irradiance * surface_tint * diffuse_weight * lighting_data.probe_volume_spacing.w *
           normal_weight * shadow_visibility;
}

float3 ProbeGIGetDebugColor(Moer::LightingData lighting_data, float3 world_pos, float3 normal) {
    if (!ProbeGIIsEnabled(lighting_data)) {
        return float3(0.0, 0.0, 0.0);
    }

    const uint debug_mode = lighting_data.probe_volume_config.y;
    if (debug_mode == 2u) {
        return ProbeGISampleIrradiance(lighting_data, world_pos, normal) * lighting_data.probe_volume_spacing.w *
               lighting_data.probe_volume_extent.w;
    }

    if (!ProbeGIIsInsideVolume(lighting_data, world_pos)) {
        return float3(0.02, 0.02, 0.02);
    }

    float3 local = ProbeGIGetLocalCoord(lighting_data, world_pos);
    float3 coord01 = saturate((world_pos - lighting_data.probe_volume_origin.xyz) / lighting_data.probe_volume_extent.xyz);
    float3 cell_frac = abs(frac(local) - 0.5);
    float grid_line = 1.0 - smoothstep(0.42, 0.49, max(max(cell_frac.x, cell_frac.y), cell_frac.z));
    float3 volume_color = lerp(coord01 * 0.35, float3(0.1, 0.85, 1.0), grid_line);
    return volume_color * lighting_data.probe_volume_extent.w;
}

#endif // MOER_RASTER_DEFERRED_LIGHTING_PROBE_GI_HLSLI
