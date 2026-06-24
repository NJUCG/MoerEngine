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

Moer::ProbeGridProbeData ProbeGILoadProbe(Moer::LightingData lighting_data, uint probe_index) {
    ArrayBuffer probe_buffer = ArrayBuffer(lighting_data.probe_volume_config.z);
    return probe_buffer.Load<Moer::ProbeGridProbeData>(probe_index);
}

float3 ProbeGILoadIrradiance(Moer::LightingData lighting_data, uint probe_index) {
    Moer::ProbeGridProbeData probe = ProbeGILoadProbe(lighting_data, probe_index);
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

float ProbeGIGetVisibilityWeight(
    Moer::LightingData lighting_data,
    Moer::ProbeGridProbeData probe,
    float3 world_pos,
    float3 normal
) {
    float3 probe_to_surface = world_pos - probe.world_position.xyz;
    float  receiver_distance = length(probe_to_surface);

    if (receiver_distance <= 1e-4) {
        return 1.0;
    }

    float3 surface_to_probe = -probe_to_surface / receiver_distance;
    float  normal_weight = saturate(dot(normal, surface_to_probe));
    normal_weight = max(normal_weight * normal_weight, lighting_data.probe_volume_visibility.z);

    float mean_distance = max(probe.visibility.x, 1e-3);
    float mean_distance_sq = max(probe.visibility.y, mean_distance * mean_distance);
    float variance = max(mean_distance_sq - mean_distance * mean_distance, 0.015);

    float biased_receiver_distance = max(receiver_distance - lighting_data.probe_volume_visibility.x, 0.0);
    float distance_delta = max(biased_receiver_distance - mean_distance, 0.0);
    float chebyshev = variance / (variance + distance_delta * distance_delta);
    chebyshev = pow(saturate(chebyshev), max(lighting_data.probe_volume_visibility.y, 0.1));

    float open_weight = lerp(lighting_data.probe_volume_visibility.z, 1.0, saturate(probe.visibility.z));
    float traced_weight = normal_weight * chebyshev * open_weight;
    return lerp(1.0, traced_weight, saturate(lighting_data.probe_volume_visibility.w * probe.visibility.w));
}

void ProbeGIAccumulateProbe(
    Moer::LightingData lighting_data,
    uint probe_index,
    float trilinear_weight,
    float3 world_pos,
    float3 normal,
    inout float3 weighted_irradiance,
    inout float visibility_weight_sum,
    inout float3 raw_irradiance
) {
    Moer::ProbeGridProbeData probe = ProbeGILoadProbe(lighting_data, probe_index);
    float3 irradiance = probe.irradiance.rgb * probe.irradiance.a;
    float visibility_weight = ProbeGIGetVisibilityWeight(lighting_data, probe, world_pos, normal);
    float final_weight = trilinear_weight * visibility_weight;

    raw_irradiance += irradiance * trilinear_weight;
    weighted_irradiance += irradiance * final_weight;
    visibility_weight_sum += final_weight;
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

    float3 weighted_irradiance = float3(0.0, 0.0, 0.0);
    float3 raw_irradiance = float3(0.0, 0.0, 0.0);
    float visibility_weight_sum = 0.0;

    ProbeGIAccumulateProbe(
        lighting_data,
        ProbeGIGetProbeIndex(base_coord, counts),
        (1.0 - weight.x) * (1.0 - weight.y) * (1.0 - weight.z),
        biased_pos,
        normal,
        weighted_irradiance,
        visibility_weight_sum,
        raw_irradiance
    );
    ProbeGIAccumulateProbe(
        lighting_data,
        ProbeGIGetProbeIndex(uint3(next_coord.x, base_coord.y, base_coord.z), counts),
        weight.x * (1.0 - weight.y) * (1.0 - weight.z),
        biased_pos,
        normal,
        weighted_irradiance,
        visibility_weight_sum,
        raw_irradiance
    );
    ProbeGIAccumulateProbe(
        lighting_data,
        ProbeGIGetProbeIndex(uint3(base_coord.x, next_coord.y, base_coord.z), counts),
        (1.0 - weight.x) * weight.y * (1.0 - weight.z),
        biased_pos,
        normal,
        weighted_irradiance,
        visibility_weight_sum,
        raw_irradiance
    );
    ProbeGIAccumulateProbe(
        lighting_data,
        ProbeGIGetProbeIndex(uint3(next_coord.x, next_coord.y, base_coord.z), counts),
        weight.x * weight.y * (1.0 - weight.z),
        biased_pos,
        normal,
        weighted_irradiance,
        visibility_weight_sum,
        raw_irradiance
    );
    ProbeGIAccumulateProbe(
        lighting_data,
        ProbeGIGetProbeIndex(uint3(base_coord.x, base_coord.y, next_coord.z), counts),
        (1.0 - weight.x) * (1.0 - weight.y) * weight.z,
        biased_pos,
        normal,
        weighted_irradiance,
        visibility_weight_sum,
        raw_irradiance
    );
    ProbeGIAccumulateProbe(
        lighting_data,
        ProbeGIGetProbeIndex(uint3(next_coord.x, base_coord.y, next_coord.z), counts),
        weight.x * (1.0 - weight.y) * weight.z,
        biased_pos,
        normal,
        weighted_irradiance,
        visibility_weight_sum,
        raw_irradiance
    );
    ProbeGIAccumulateProbe(
        lighting_data,
        ProbeGIGetProbeIndex(uint3(base_coord.x, next_coord.y, next_coord.z), counts),
        (1.0 - weight.x) * weight.y * weight.z,
        biased_pos,
        normal,
        weighted_irradiance,
        visibility_weight_sum,
        raw_irradiance
    );
    ProbeGIAccumulateProbe(
        lighting_data,
        ProbeGIGetProbeIndex(next_coord, counts),
        weight.x * weight.y * weight.z,
        biased_pos,
        normal,
        weighted_irradiance,
        visibility_weight_sum,
        raw_irradiance
    );

    return lerp(raw_irradiance, weighted_irradiance, saturate(lighting_data.probe_volume_visibility.w));
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
    float diffuse_weight = 0.15 + 0.85 * (1.0 - saturate(metallic));
    float normal_weight = 0.35 + 0.65 * saturate(normal.y * 0.5 + 0.5);
    float shadow_fill = lerp(0.42, 0.08, saturate(direct_shadow));
    float3 surface_tint = lerp(float3(1.0, 1.0, 1.0), albedo, 0.85);
    return irradiance * surface_tint * diffuse_weight * lighting_data.probe_volume_spacing.w *
           normal_weight * shadow_fill;
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
    if (debug_mode == 4u) {
        if (!ProbeGIIsInsideVolume(lighting_data, world_pos)) {
            return float3(0.02, 0.02, 0.02);
        }

        uint3 counts = ProbeGIGetCounts(lighting_data);
        float3 local = ProbeGIGetLocalCoord(lighting_data, world_pos);
        uint3 coord = min(uint3(round(clamp(local, float3(0.0, 0.0, 0.0), float3(counts - uint3(1, 1, 1))))), counts - uint3(1, 1, 1));
        Moer::ProbeGridProbeData probe = ProbeGILoadProbe(lighting_data, ProbeGIGetProbeIndex(coord, counts));
        float open_ratio = saturate(probe.visibility.z);
        float mean_distance = saturate(probe.visibility.x / max(max(lighting_data.probe_volume_extent.x, lighting_data.probe_volume_extent.y), lighting_data.probe_volume_extent.z));
        return float3(1.0 - open_ratio, open_ratio, mean_distance) * lighting_data.probe_volume_extent.w;
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
