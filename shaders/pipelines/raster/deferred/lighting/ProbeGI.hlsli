#ifndef MOER_RASTER_DEFERRED_LIGHTING_PROBE_GI_HLSLI
#define MOER_RASTER_DEFERRED_LIGHTING_PROBE_GI_HLSLI

#include "shared/raster/ShaderParameters.h"

bool ProbeGIIsEnabled(Moer::LightingData lighting_data) {
    return lighting_data.probe_system_config.x != 0u && lighting_data.probe_system_config.z != 0u &&
           lighting_data.probe_system_config.w != 0u && lighting_data.probe_system_counts.x > 0u &&
           lighting_data.probe_system_counts.y > 0u && lighting_data.probe_system_counts.z != 0u &&
           lighting_data.probe_system_counts.w != 0u && lighting_data.probe_system_hierarchy.x != 0u &&
           lighting_data.probe_system_hierarchy.y > 0u;
}

Moer::ProbeVolumeGpuDesc ProbeGILoadVolume(Moer::LightingData lighting_data, uint volume_index) {
    ArrayBuffer volume_buffer = ArrayBuffer(lighting_data.probe_system_config.w);
    return volume_buffer.Load<Moer::ProbeVolumeGpuDesc>(volume_index);
}

uint3 ProbeGIGetCounts(Moer::ProbeVolumeGpuDesc volume) {
    return uint3(
        max(volume.counts.x, 1u),
        max(volume.counts.y, 1u),
        max(volume.counts.z, 1u)
    );
}

uint ProbeGIGetLevelScale(uint subdivision_level) {
    return 1u << min(subdivision_level, Moer::RASTER_PROBE_MAX_SUBDIVISION_LEVEL);
}

uint3 ProbeGIGetLevelCounts(Moer::ProbeVolumeGpuDesc volume, uint subdivision_level) {
    const uint scale = ProbeGIGetLevelScale(subdivision_level);
    return max(
        (ProbeGIGetCounts(volume) + uint3(scale - 1u, scale - 1u, scale - 1u)) / scale,
        uint3(1u, 1u, 1u)
    );
}

float3 ProbeGIGetLevelSpacing(Moer::ProbeVolumeGpuDesc volume, uint subdivision_level) {
    const uint3 counts = ProbeGIGetLevelCounts(volume, subdivision_level);
    return float3(
        counts.x > 1u ? volume.extent_blend.x / float(counts.x - 1u) : volume.extent_blend.x,
        counts.y > 1u ? volume.extent_blend.y / float(counts.y - 1u) : volume.extent_blend.y,
        counts.z > 1u ? volume.extent_blend.z / float(counts.z - 1u) : volume.extent_blend.z
    );
}

uint ProbeGIGetLevelGridDim(uint subdivision_level) {
    return max(
        Moer::RASTER_PROBE_MAX_FINE_BRICKS_PER_AXIS >>
            min(subdivision_level, Moer::RASTER_PROBE_MAX_SUBDIVISION_LEVEL),
        1u
    );
}

uint ProbeGIGetLevelPageOffset(uint subdivision_level) {
    uint page_offset = 0u;
    [unroll] for (uint level = 0u; level < Moer::RASTER_PROBE_MAX_SUBDIVISION_LEVEL; ++level) {
        if (level < subdivision_level) {
            const uint dim = ProbeGIGetLevelGridDim(level);
            page_offset += dim * dim * dim;
        }
    }
    return page_offset;
}

uint ProbeGIGetBrickIndexAtLevel(
    Moer::LightingData lighting_data,
    Moer::ProbeVolumeGpuDesc volume,
    uint3 coord,
    uint subdivision_level
) {
    const uint level = min(subdivision_level, volume.hierarchy.z);
    const uint3 counts = ProbeGIGetLevelCounts(volume, level);
    if (any(coord >= counts)) {
        return Moer::RASTER_PROBE_PAGE_INVALID;
    }

    const uint3 brick_coord = coord / Moer::RASTER_PROBE_BRICK_DIM;
    const uint level_grid_dim = ProbeGIGetLevelGridDim(level);
    const uint virtual_page = volume.allocation.z + ProbeGIGetLevelPageOffset(level) +
                              brick_coord.x + brick_coord.y * level_grid_dim +
                              brick_coord.z * level_grid_dim * level_grid_dim;

    ArrayBuffer page_table = ArrayBuffer(lighting_data.probe_system_counts.w);
    const uint brick_index = page_table.Load<uint>(virtual_page);
    if (brick_index == Moer::RASTER_PROBE_PAGE_INVALID) {
        return Moer::RASTER_PROBE_PAGE_INVALID;
    }

    ArrayBuffer brick_buffer = ArrayBuffer(lighting_data.probe_system_counts.z);
    const Moer::ProbeBrickGpuDesc brick = brick_buffer.Load<Moer::ProbeBrickGpuDesc>(brick_index);
    const bool descriptor_matches = brick.probe_range.z != 0u && brick.hierarchy.y == level &&
                                    brick.hierarchy.w == virtual_page &&
                                    brick.neighbor_pages_1.z == lighting_data.probe_system_hierarchy.w;
    return descriptor_matches ? brick_index : Moer::RASTER_PROBE_PAGE_INVALID;
}

uint ProbeGIGetBrickIndex(
    Moer::LightingData lighting_data,
    Moer::ProbeVolumeGpuDesc volume,
    uint3 coord
) {
    return ProbeGIGetBrickIndexAtLevel(lighting_data, volume, coord, 0u);
}

uint3 ProbeGIGetCellCounts(Moer::ProbeVolumeGpuDesc volume) {
    const uint3 counts = ProbeGIGetCounts(volume);
    const uint3 fine_brick_counts =
        (counts + uint3(
            Moer::RASTER_PROBE_BRICK_DIM - 1u,
            Moer::RASTER_PROBE_BRICK_DIM - 1u,
            Moer::RASTER_PROBE_BRICK_DIM - 1u
        )) / Moer::RASTER_PROBE_BRICK_DIM;
    return (fine_brick_counts + uint3(
            Moer::RASTER_PROBE_CELL_BRICK_DIM - 1u,
            Moer::RASTER_PROBE_CELL_BRICK_DIM - 1u,
            Moer::RASTER_PROBE_CELL_BRICK_DIM - 1u
        )) /
        Moer::RASTER_PROBE_CELL_BRICK_DIM;
}

uint3 ProbeGIGetCellCoord(uint3 coord) {
    return (coord / Moer::RASTER_PROBE_BRICK_DIM) / Moer::RASTER_PROBE_CELL_BRICK_DIM;
}

uint ProbeGIGetCellIndex(Moer::ProbeVolumeGpuDesc volume, uint3 coord) {
    const uint3 cell_counts = ProbeGIGetCellCounts(volume);
    const uint3 cell_coord =
        min(ProbeGIGetCellCoord(coord), cell_counts - uint3(1u, 1u, 1u));
    const uint local_cell_index =
        cell_coord.x + cell_coord.y * cell_counts.x + cell_coord.z * cell_counts.x * cell_counts.y;
    return local_cell_index < volume.hierarchy.y ?
               volume.hierarchy.x + local_cell_index :
               Moer::RASTER_PROBE_PAGE_INVALID;
}

uint ProbeGIGetProbeIndexAtLevel(
    Moer::LightingData lighting_data,
    Moer::ProbeVolumeGpuDesc volume,
    uint3 coord,
    uint subdivision_level
) {
    const uint brick_index =
        ProbeGIGetBrickIndexAtLevel(lighting_data, volume, coord, subdivision_level);
    if (brick_index == Moer::RASTER_PROBE_PAGE_INVALID) {
        return Moer::RASTER_PROBE_PAGE_INVALID;
    }

    const uint3 brick_coord = coord / Moer::RASTER_PROBE_BRICK_DIM;
    ArrayBuffer brick_buffer = ArrayBuffer(lighting_data.probe_system_counts.z);
    const Moer::ProbeBrickGpuDesc brick = brick_buffer.Load<Moer::ProbeBrickGpuDesc>(brick_index);
    const uint3 local_coord = coord - brick_coord * Moer::RASTER_PROBE_BRICK_DIM;
    if (any(local_coord >= brick.local_counts.xyz)) {
        return Moer::RASTER_PROBE_PAGE_INVALID;
    }

    const uint local_index = local_coord.x + local_coord.y * brick.local_counts.x +
                             local_coord.z * brick.local_counts.x * brick.local_counts.y;
    return brick.probe_range.x + local_index;
}

uint ProbeGIGetProbeIndex(
    Moer::LightingData lighting_data,
    Moer::ProbeVolumeGpuDesc volume,
    uint3 coord
) {
    return ProbeGIGetProbeIndexAtLevel(lighting_data, volume, coord, 0u);
}

Moer::ProbeGridProbeData ProbeGILoadProbe(Moer::LightingData lighting_data, uint probe_index) {
    ArrayBuffer probe_buffer = ArrayBuffer(lighting_data.probe_system_config.z);
    return probe_buffer.Load<Moer::ProbeGridProbeData>(probe_index);
}

uint ProbeGIGetProbeState(Moer::ProbeGridProbeData probe) {
    return uint(round(max(probe.world_position.w, 0.0)));
}

float ProbeGIGetProbeStateWeight(Moer::ProbeGridProbeData probe) {
    const uint state = ProbeGIGetProbeState(probe);
    if (state == Moer::RASTER_PROBE_STATE_INVALID) {
        return 0.0;
    }
    if (state == Moer::RASTER_PROBE_STATE_NEAR_SURFACE) {
        return 0.35;
    }
    return 1.0;
}

float2 ProbeGIEncodeOctahedral(float3 direction) {
    float3 n = direction / max(abs(direction.x) + abs(direction.y) + abs(direction.z), 1e-5);
    if (n.z < 0.0) {
        float2 sign_xy = float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
        n.xy = (1.0 - abs(n.yx)) * sign_xy;
    }
    return saturate(n.xy * 0.5 + 0.5);
}

uint ProbeGIGetVisibilityAtlasIndex(uint probe_index, float3 probe_to_surface_dir) {
    const uint dim = Moer::RASTER_PROBE_VISIBILITY_ATLAS_DIM;
    float2 uv = ProbeGIEncodeOctahedral(probe_to_surface_dir);
    uint2 texel = min(uint2(uv * float(dim)), uint2(dim - 1u, dim - 1u));
    return probe_index * Moer::RASTER_PROBE_VISIBILITY_ATLAS_TEXEL_COUNT + texel.y * dim + texel.x;
}

uint ProbeGIGetIrradianceAtlasIndex(uint probe_index, float3 sample_direction) {
    const uint dim = Moer::RASTER_PROBE_IRRADIANCE_ATLAS_DIM;
    float2 uv = ProbeGIEncodeOctahedral(sample_direction);
    uint2 texel = min(uint2(uv * float(dim)), uint2(dim - 1u, dim - 1u));
    return probe_index * Moer::RASTER_PROBE_IRRADIANCE_ATLAS_TEXEL_COUNT + texel.y * dim + texel.x;
}

int2 ProbeGIFoldOctahedralTexel(int2 texel, uint dim) {
    const int max_texel = int(dim) - 1;
    int2 folded = texel;

    [unroll] for (uint fold_index = 0u; fold_index < 2u; ++fold_index) {
        if (folded.x < 0) {
            folded.x = max_texel;
            folded.y = max_texel - folded.y;
        } else if (folded.x > max_texel) {
            folded.x = 0;
            folded.y = max_texel - folded.y;
        }

        if (folded.y < 0) {
            folded.y = max_texel;
            folded.x = max_texel - folded.x;
        } else if (folded.y > max_texel) {
            folded.y = 0;
            folded.x = max_texel - folded.x;
        }
    }

    return clamp(folded, int2(0, 0), int2(max_texel, max_texel));
}

uint ProbeGIGetAtlasTexelOffset(int2 texel, uint dim) {
    const int2 folded = ProbeGIFoldOctahedralTexel(texel, dim);
    return uint(folded.y) * dim + uint(folded.x);
}

float2 ProbeGIGetAtlasTexelPosition(float3 direction, uint dim) {
    return ProbeGIEncodeOctahedral(direction) * float(dim) - 0.5;
}

float2 ProbeGIGetAtlasTextureUv(uint probe_index, float3 direction) {
    const float2 oct_uv = ProbeGIEncodeOctahedral(direction);
    const uint tile_x = probe_index % Moer::RASTER_PROBE_ATLAS_TILE_COLUMNS;
    const uint tile_y = probe_index / Moer::RASTER_PROBE_ATLAS_TILE_COLUMNS;
    const float2 tile_origin = float2(tile_x, tile_y) * float(Moer::RASTER_PROBE_ATLAS_TILE_STRIDE);
    const float2 atlas_pixel =
        tile_origin + float(Moer::RASTER_PROBE_ATLAS_TILE_BORDER) +
        oct_uv * float(Moer::RASTER_PROBE_IRRADIANCE_ATLAS_DIM);
    return atlas_pixel /
           float2(Moer::RASTER_PROBE_ATLAS_TEXTURE_WIDTH, Moer::RASTER_PROBE_ATLAS_TEXTURE_HEIGHT);
}

float4 ProbeGILoadVisibilityAtlasTexel(ArrayBuffer visibility_buffer, uint probe_index, int2 texel) {
    const uint dim = Moer::RASTER_PROBE_VISIBILITY_ATLAS_DIM;
    const uint atlas_index =
        probe_index * Moer::RASTER_PROBE_VISIBILITY_ATLAS_TEXEL_COUNT + ProbeGIGetAtlasTexelOffset(texel, dim);
    return visibility_buffer.Load<Moer::ProbeGridVisibilityTexel>(atlas_index).moments;
}

float4 ProbeGILoadIrradianceAtlasTexel(ArrayBuffer irradiance_buffer, uint probe_index, int2 texel) {
    const uint dim = Moer::RASTER_PROBE_IRRADIANCE_ATLAS_DIM;
    const uint atlas_index =
        probe_index * Moer::RASTER_PROBE_IRRADIANCE_ATLAS_TEXEL_COUNT + ProbeGIGetAtlasTexelOffset(texel, dim);
    return irradiance_buffer.Load<Moer::ProbeGridIrradianceTexel>(atlas_index).irradiance;
}

float4 ProbeGISampleVisibilityAtlasBilinear(ArrayBuffer visibility_buffer, uint probe_index, float3 probe_to_surface_dir) {
    const uint dim = Moer::RASTER_PROBE_VISIBILITY_ATLAS_DIM;
    const float2 texel_pos = ProbeGIGetAtlasTexelPosition(probe_to_surface_dir, dim);
    const int2 base_texel = int2(floor(texel_pos));
    const float2 frac_texel = saturate(texel_pos - float2(base_texel));

    const float w00 = (1.0 - frac_texel.x) * (1.0 - frac_texel.y);
    const float w10 = frac_texel.x * (1.0 - frac_texel.y);
    const float w01 = (1.0 - frac_texel.x) * frac_texel.y;
    const float w11 = frac_texel.x * frac_texel.y;

    const float4 v00 = ProbeGILoadVisibilityAtlasTexel(visibility_buffer, probe_index, base_texel);
    const float4 v10 = ProbeGILoadVisibilityAtlasTexel(visibility_buffer, probe_index, base_texel + int2(1, 0));
    const float4 v01 = ProbeGILoadVisibilityAtlasTexel(visibility_buffer, probe_index, base_texel + int2(0, 1));
    const float4 v11 = ProbeGILoadVisibilityAtlasTexel(visibility_buffer, probe_index, base_texel + int2(1, 1));

    const float confidence = saturate(v00.w * w00 + v10.w * w10 + v01.w * w01 + v11.w * w11);
    if (confidence <= 1e-4) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    const float3 moments =
        (v00.xyz * v00.w * w00 + v10.xyz * v10.w * w10 + v01.xyz * v01.w * w01 + v11.xyz * v11.w * w11) /
        confidence;
    return float4(moments, confidence);
}

float4 ProbeGISampleIrradianceAtlasBilinear(ArrayBuffer irradiance_buffer, uint probe_index, float3 sample_direction) {
    const uint dim = Moer::RASTER_PROBE_IRRADIANCE_ATLAS_DIM;
    const float2 texel_pos = ProbeGIGetAtlasTexelPosition(sample_direction, dim);
    const int2 base_texel = int2(floor(texel_pos));
    const float2 frac_texel = saturate(texel_pos - float2(base_texel));

    const float w00 = (1.0 - frac_texel.x) * (1.0 - frac_texel.y);
    const float w10 = frac_texel.x * (1.0 - frac_texel.y);
    const float w01 = (1.0 - frac_texel.x) * frac_texel.y;
    const float w11 = frac_texel.x * frac_texel.y;

    const float4 i00 = ProbeGILoadIrradianceAtlasTexel(irradiance_buffer, probe_index, base_texel);
    const float4 i10 = ProbeGILoadIrradianceAtlasTexel(irradiance_buffer, probe_index, base_texel + int2(1, 0));
    const float4 i01 = ProbeGILoadIrradianceAtlasTexel(irradiance_buffer, probe_index, base_texel + int2(0, 1));
    const float4 i11 = ProbeGILoadIrradianceAtlasTexel(irradiance_buffer, probe_index, base_texel + int2(1, 1));

    const float confidence = saturate(i00.w * w00 + i10.w * w10 + i01.w * w01 + i11.w * w11);
    if (confidence <= 1e-4) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    const float3 irradiance =
        (i00.rgb * i00.w * w00 + i10.rgb * i10.w * w10 + i01.rgb * i01.w * w01 + i11.rgb * i11.w * w11) /
        confidence;
    return float4(irradiance, confidence);
}

float4 ProbeGILoadDirectionalVisibility(
    Moer::LightingData lighting_data,
    uint probe_index,
    Moer::ProbeGridProbeData probe,
    float3 probe_to_surface_dir
) {
    if (lighting_data.probe_system_atlas.w != 0u) {
        TextureHandle visibility_texture = TextureHandle(lighting_data.probe_system_atlas.w);
        const float2 atlas_uv = ProbeGIGetAtlasTextureUv(probe_index, probe_to_surface_dir);
        const float4 visibility_moments = visibility_texture.Sample2D<float4>(atlas_uv);
        return lerp(probe.visibility, visibility_moments, saturate(visibility_moments.w));
    }

    if (lighting_data.probe_system_atlas.x != 0u) {
        ArrayBuffer visibility_buffer = ArrayBuffer(lighting_data.probe_system_atlas.x);
        const float4 visibility_moments =
            ProbeGISampleVisibilityAtlasBilinear(visibility_buffer, probe_index, probe_to_surface_dir);
        return lerp(probe.visibility, visibility_moments, saturate(visibility_moments.w));
    }

    return probe.visibility;
}

float3 ProbeGILoadIrradiance(Moer::LightingData lighting_data, uint probe_index) {
    Moer::ProbeGridProbeData probe = ProbeGILoadProbe(lighting_data, probe_index);
    return probe.irradiance.rgb * probe.irradiance.a;
}

float3 ProbeGILoadDirectionalIrradiance(
    Moer::LightingData lighting_data,
    uint probe_index,
    Moer::ProbeGridProbeData probe,
    float3 normal
) {
    const float3 aggregate_irradiance = probe.irradiance.rgb * probe.irradiance.a;
    if (lighting_data.probe_system_atlas.z != 0u) {
        TextureHandle irradiance_texture = TextureHandle(lighting_data.probe_system_atlas.z);
        const float2 atlas_uv = ProbeGIGetAtlasTextureUv(probe_index, normalize(normal));
        const float4 irradiance_texel = irradiance_texture.Sample2D<float4>(atlas_uv);
        return lerp(aggregate_irradiance, irradiance_texel.rgb, saturate(irradiance_texel.w));
    }

    if (lighting_data.probe_system_atlas.y != 0u) {
        ArrayBuffer irradiance_buffer = ArrayBuffer(lighting_data.probe_system_atlas.y);
        const float4 irradiance_texel =
            ProbeGISampleIrradianceAtlasBilinear(irradiance_buffer, probe_index, normalize(normal));
        return lerp(aggregate_irradiance, irradiance_texel.rgb, saturate(irradiance_texel.w));
    }

    return aggregate_irradiance;
}

float3 ProbeGIGetLocalCoordAtLevel(
    Moer::ProbeVolumeGpuDesc volume,
    float3 world_pos,
    uint subdivision_level
) {
    const float3 spacing = max(
        ProbeGIGetLevelSpacing(volume, subdivision_level),
        float3(1e-4, 1e-4, 1e-4)
    );
    return (world_pos - volume.origin_bias.xyz) / spacing;
}

float3 ProbeGIGetLocalCoord(Moer::ProbeVolumeGpuDesc volume, float3 world_pos) {
    return ProbeGIGetLocalCoordAtLevel(volume, world_pos, 0u);
}

bool ProbeGIIsInsideVolume(Moer::ProbeVolumeGpuDesc volume, float3 world_pos) {
    float3 volume_min = volume.origin_bias.xyz;
    float3 volume_max = volume_min + volume.extent_blend.xyz;
    return all(world_pos >= volume_min) && all(world_pos <= volume_max);
}

float ProbeGIGetVolumeBlendWeight(Moer::ProbeVolumeGpuDesc volume, float3 world_pos) {
    const float3 distance_to_min = world_pos - volume.origin_bias.xyz;
    const float3 distance_to_max = volume.origin_bias.xyz + volume.extent_blend.xyz - world_pos;
    const float edge_distance = min(
        min(distance_to_min.x, min(distance_to_min.y, distance_to_min.z)),
        min(distance_to_max.x, min(distance_to_max.y, distance_to_max.z))
    );
    const float edge_fade = saturate(edge_distance / max(volume.extent_blend.w, 0.01));
    const float max_spacing = max(
        volume.spacing_intensity.x,
        max(volume.spacing_intensity.y, volume.spacing_intensity.z)
    );
    const float density = rcp(max(max_spacing, 1e-3));
    return max(edge_fade, 0.01) * density * density;
}

float ProbeGIGetVisibilityWeight(
    Moer::LightingData lighting_data,
    Moer::ProbeVolumeGpuDesc volume,
    uint probe_index,
    Moer::ProbeGridProbeData probe,
    float3 world_pos,
    float3 normal
) {
    float3 probe_to_surface = world_pos - probe.world_position.xyz;
    float  receiver_distance = length(probe_to_surface);

    if (receiver_distance <= 1e-4) {
        return 1.0;
    }

    float3 probe_to_surface_dir = probe_to_surface / receiver_distance;
    float3 surface_to_probe = -probe_to_surface_dir;
    float  normal_weight = saturate(dot(normal, surface_to_probe));
    normal_weight = max(normal_weight * normal_weight, volume.visibility.z);

    float4 directional_visibility =
        ProbeGILoadDirectionalVisibility(lighting_data, probe_index, probe, probe_to_surface_dir);

    float mean_distance = max(directional_visibility.x, 1e-3);
    float mean_distance_sq = max(directional_visibility.y, mean_distance * mean_distance);
    float variance = max(mean_distance_sq - mean_distance * mean_distance, 0.015);

    float biased_receiver_distance = max(receiver_distance - volume.visibility.x, 0.0);
    float distance_delta = max(biased_receiver_distance - mean_distance, 0.0);
    float chebyshev = variance / (variance + distance_delta * distance_delta);
    chebyshev = pow(saturate(chebyshev), max(volume.visibility.y, 0.1));

    float open_weight = lerp(volume.visibility.z, 1.0, saturate(directional_visibility.z));
    float traced_weight = normal_weight * chebyshev * open_weight;
    return lerp(1.0, traced_weight, saturate(volume.visibility.w * directional_visibility.w));
}

void ProbeGIAccumulateProbe(
    Moer::LightingData lighting_data,
    Moer::ProbeVolumeGpuDesc volume,
    uint probe_index,
    float trilinear_weight,
    float3 world_pos,
    float3 normal,
    inout float3 weighted_irradiance,
    inout float resident_weight_sum,
    inout float3 raw_irradiance
) {
    if (probe_index == Moer::RASTER_PROBE_PAGE_INVALID || trilinear_weight <= 0.0) {
        return;
    }

    Moer::ProbeGridProbeData probe = ProbeGILoadProbe(lighting_data, probe_index);
    float state_weight = ProbeGIGetProbeStateWeight(probe);
    if (state_weight <= 0.0) {
        return;
    }

    const float usable_weight = trilinear_weight * state_weight;
    resident_weight_sum += usable_weight;

    float3 irradiance = ProbeGILoadDirectionalIrradiance(lighting_data, probe_index, probe, normal);
    float visibility_weight = ProbeGIGetVisibilityWeight(lighting_data, volume, probe_index, probe, world_pos, normal);
    float final_weight = usable_weight * visibility_weight;

    raw_irradiance += irradiance * usable_weight;
    weighted_irradiance += irradiance * final_weight;
}

float3 ProbeGISampleVolumeLevelIrradiance(
    Moer::LightingData lighting_data,
    Moer::ProbeVolumeGpuDesc volume,
    float3 world_pos,
    float3 normal,
    uint subdivision_level,
    out float resident_coverage
) {
    resident_coverage = 0.0;
    float3 biased_pos = world_pos + normal * volume.origin_bias.w;
    if (!ProbeGIIsInsideVolume(volume, biased_pos)) {
        return float3(0.0, 0.0, 0.0);
    }

    const uint level = min(subdivision_level, volume.hierarchy.z);
    uint3 counts = ProbeGIGetLevelCounts(volume, level);
    uint3 max_coord = counts - uint3(1, 1, 1);
    float3 max_coord_f = float3(max_coord.x, max_coord.y, max_coord.z);
    float3 local = clamp(
        ProbeGIGetLocalCoordAtLevel(volume, biased_pos, level),
        float3(0.0, 0.0, 0.0),
        max_coord_f
    );

    uint3 base_coord = uint3(floor(local));
    uint3 next_coord = min(base_coord + uint3(1, 1, 1), max_coord);
    float3 weight = frac(local);

    float3 weighted_irradiance = float3(0.0, 0.0, 0.0);
    float3 raw_irradiance = float3(0.0, 0.0, 0.0);
    float resident_weight_sum = 0.0;

    ProbeGIAccumulateProbe(
        lighting_data,
        volume,
        ProbeGIGetProbeIndexAtLevel(lighting_data, volume, base_coord, level),
        (1.0 - weight.x) * (1.0 - weight.y) * (1.0 - weight.z),
        biased_pos,
        normal,
        weighted_irradiance,
        resident_weight_sum,
        raw_irradiance
    );
    ProbeGIAccumulateProbe(
        lighting_data,
        volume,
        ProbeGIGetProbeIndexAtLevel(
            lighting_data,
            volume,
            uint3(next_coord.x, base_coord.y, base_coord.z),
            level
        ),
        weight.x * (1.0 - weight.y) * (1.0 - weight.z),
        biased_pos,
        normal,
        weighted_irradiance,
        resident_weight_sum,
        raw_irradiance
    );
    ProbeGIAccumulateProbe(
        lighting_data,
        volume,
        ProbeGIGetProbeIndexAtLevel(
            lighting_data,
            volume,
            uint3(base_coord.x, next_coord.y, base_coord.z),
            level
        ),
        (1.0 - weight.x) * weight.y * (1.0 - weight.z),
        biased_pos,
        normal,
        weighted_irradiance,
        resident_weight_sum,
        raw_irradiance
    );
    ProbeGIAccumulateProbe(
        lighting_data,
        volume,
        ProbeGIGetProbeIndexAtLevel(
            lighting_data,
            volume,
            uint3(next_coord.x, next_coord.y, base_coord.z),
            level
        ),
        weight.x * weight.y * (1.0 - weight.z),
        biased_pos,
        normal,
        weighted_irradiance,
        resident_weight_sum,
        raw_irradiance
    );
    ProbeGIAccumulateProbe(
        lighting_data,
        volume,
        ProbeGIGetProbeIndexAtLevel(
            lighting_data,
            volume,
            uint3(base_coord.x, base_coord.y, next_coord.z),
            level
        ),
        (1.0 - weight.x) * (1.0 - weight.y) * weight.z,
        biased_pos,
        normal,
        weighted_irradiance,
        resident_weight_sum,
        raw_irradiance
    );
    ProbeGIAccumulateProbe(
        lighting_data,
        volume,
        ProbeGIGetProbeIndexAtLevel(
            lighting_data,
            volume,
            uint3(next_coord.x, base_coord.y, next_coord.z),
            level
        ),
        weight.x * (1.0 - weight.y) * weight.z,
        biased_pos,
        normal,
        weighted_irradiance,
        resident_weight_sum,
        raw_irradiance
    );
    ProbeGIAccumulateProbe(
        lighting_data,
        volume,
        ProbeGIGetProbeIndexAtLevel(
            lighting_data,
            volume,
            uint3(base_coord.x, next_coord.y, next_coord.z),
            level
        ),
        (1.0 - weight.x) * weight.y * weight.z,
        biased_pos,
        normal,
        weighted_irradiance,
        resident_weight_sum,
        raw_irradiance
    );
    ProbeGIAccumulateProbe(
        lighting_data,
        volume,
        ProbeGIGetProbeIndexAtLevel(lighting_data, volume, next_coord, level),
        weight.x * weight.y * weight.z,
        biased_pos,
        normal,
        weighted_irradiance,
        resident_weight_sum,
        raw_irradiance
    );

    resident_coverage = saturate(resident_weight_sum);
    if (resident_coverage <= 1e-5) {
        return float3(0.0, 0.0, 0.0);
    }

    const float inverse_coverage = rcp(resident_coverage);
    raw_irradiance *= inverse_coverage;
    weighted_irradiance *= inverse_coverage;
    return lerp(raw_irradiance, weighted_irradiance, saturate(volume.visibility.w));
}

void ProbeGIConsiderCoarserNeighbor(
    Moer::LightingData lighting_data,
    Moer::ProbeVolumeGpuDesc volume,
    Moer::ProbeCellGpuDesc cell,
    uint3 cell_counts,
    int3 neighbor_offset,
    float3 biased_pos,
    uint desired_level,
    inout uint transition_level,
    inout float transition_weight
) {
    const int3 neighbor_coord = int3(cell.coord_volume.xyz) + neighbor_offset;
    if (any(neighbor_coord < int3(0, 0, 0)) || any(neighbor_coord >= int3(cell_counts))) {
        return;
    }

    const uint3 neighbor_coord_u = uint3(neighbor_coord);
    const uint local_neighbor_index = neighbor_coord_u.x + neighbor_coord_u.y * cell_counts.x +
                                      neighbor_coord_u.z * cell_counts.x * cell_counts.y;
    if (local_neighbor_index >= volume.hierarchy.y) {
        return;
    }

    ArrayBuffer cell_buffer = ArrayBuffer(lighting_data.probe_system_hierarchy.x);
    const Moer::ProbeCellGpuDesc neighbor =
        cell_buffer.Load<Moer::ProbeCellGpuDesc>(volume.hierarchy.x + local_neighbor_index);
    const uint neighbor_level = min(neighbor.geometry.z, volume.hierarchy.z);
    if (neighbor.coord_volume.w != cell.coord_volume.w || neighbor_level <= desired_level) {
        return;
    }

    const uint axis = neighbor_offset.x != 0 ? 0u : (neighbor_offset.y != 0 ? 1u : 2u);
    const bool positive_side = neighbor_offset[axis] > 0;
    const float current_edge = positive_side ?
                                   cell.origin_spacing[axis] + cell.extent[axis] :
                                   cell.origin_spacing[axis];
    const float neighbor_edge = positive_side ?
                                    neighbor.origin_spacing[axis] :
                                    neighbor.origin_spacing[axis] + neighbor.extent[axis];
    const float boundary = 0.5 * (current_edge + neighbor_edge);
    const float transition_scale = max(lighting_data.probe_system_debug.y, 0.0);
    if (transition_scale <= 1e-5) {
        return;
    }

    const float transition_width = max(volume.spacing_intensity[axis] * transition_scale, 1e-4);
    const float candidate_weight =
        1.0 - smoothstep(0.0, transition_width, abs(biased_pos[axis] - boundary));
    if (candidate_weight > transition_weight + 1e-5 ||
        (abs(candidate_weight - transition_weight) <= 1e-5 && neighbor_level > transition_level)) {
        transition_level = neighbor_level;
        transition_weight = candidate_weight;
    }
}

void ProbeGIResolveAdaptiveLevels(
    Moer::LightingData lighting_data,
    Moer::ProbeVolumeGpuDesc volume,
    float3 biased_pos,
    out uint desired_level,
    out uint transition_level,
    out float transition_weight
) {
    desired_level = 0u;
    transition_level = 0u;
    transition_weight = 0.0;
    if (volume.hierarchy.z == 0u || volume.hierarchy.y == 0u ||
        lighting_data.probe_system_hierarchy.x == 0u) {
        return;
    }

    const uint3 counts = ProbeGIGetCounts(volume);
    const uint3 max_coord = counts - uint3(1u, 1u, 1u);
    const float3 local = clamp(
        ProbeGIGetLocalCoord(volume, biased_pos),
        float3(0.0, 0.0, 0.0),
        float3(max_coord)
    );
    const uint3 coord = min(uint3(floor(local)), max_coord);
    const uint cell_index = ProbeGIGetCellIndex(volume, coord);
    if (cell_index == Moer::RASTER_PROBE_PAGE_INVALID ||
        cell_index >= lighting_data.probe_system_hierarchy.y) {
        return;
    }

    ArrayBuffer cell_buffer = ArrayBuffer(lighting_data.probe_system_hierarchy.x);
    const Moer::ProbeCellGpuDesc cell = cell_buffer.Load<Moer::ProbeCellGpuDesc>(cell_index);
    desired_level = min(cell.geometry.z, volume.hierarchy.z);
    transition_level = desired_level;

    const uint3 cell_counts = ProbeGIGetCellCounts(volume);
    ProbeGIConsiderCoarserNeighbor(
        lighting_data,
        volume,
        cell,
        cell_counts,
        int3(-1, 0, 0),
        biased_pos,
        desired_level,
        transition_level,
        transition_weight
    );
    ProbeGIConsiderCoarserNeighbor(
        lighting_data,
        volume,
        cell,
        cell_counts,
        int3(1, 0, 0),
        biased_pos,
        desired_level,
        transition_level,
        transition_weight
    );
    ProbeGIConsiderCoarserNeighbor(
        lighting_data,
        volume,
        cell,
        cell_counts,
        int3(0, -1, 0),
        biased_pos,
        desired_level,
        transition_level,
        transition_weight
    );
    ProbeGIConsiderCoarserNeighbor(
        lighting_data,
        volume,
        cell,
        cell_counts,
        int3(0, 1, 0),
        biased_pos,
        desired_level,
        transition_level,
        transition_weight
    );
    ProbeGIConsiderCoarserNeighbor(
        lighting_data,
        volume,
        cell,
        cell_counts,
        int3(0, 0, -1),
        biased_pos,
        desired_level,
        transition_level,
        transition_weight
    );
    ProbeGIConsiderCoarserNeighbor(
        lighting_data,
        volume,
        cell,
        cell_counts,
        int3(0, 0, 1),
        biased_pos,
        desired_level,
        transition_level,
        transition_weight
    );
}

float3 ProbeGISampleHierarchyFromLevel(
    Moer::LightingData lighting_data,
    Moer::ProbeVolumeGpuDesc volume,
    float3 world_pos,
    float3 normal,
    uint start_level,
    out float resolved_coverage,
    out float resolved_level
) {
    float3 irradiance_sum = float3(0.0, 0.0, 0.0);
    float remaining_coverage = 1.0;
    float level_weight_sum = 0.0;
    resolved_coverage = 0.0;
    resolved_level = float(min(start_level, volume.hierarchy.z));

    [unroll] for (uint level = 0u; level <= Moer::RASTER_PROBE_MAX_SUBDIVISION_LEVEL; ++level) {
        if (level < start_level || level > volume.hierarchy.z || remaining_coverage <= 1e-5) {
            continue;
        }

        float level_coverage = 0.0;
        const float3 level_irradiance = ProbeGISampleVolumeLevelIrradiance(
            lighting_data,
            volume,
            world_pos,
            normal,
            level,
            level_coverage
        );
        const float contribution = remaining_coverage * saturate(level_coverage);
        irradiance_sum += level_irradiance * contribution;
        level_weight_sum += float(level) * contribution;
        resolved_coverage += contribution;
        remaining_coverage *= 1.0 - saturate(level_coverage);
    }

    if (resolved_coverage <= 1e-5) {
        resolved_coverage = 0.0;
        return float3(0.0, 0.0, 0.0);
    }

    const float inverse_coverage = rcp(resolved_coverage);
    resolved_level = level_weight_sum * inverse_coverage;
    return irradiance_sum * inverse_coverage;
}

float3 ProbeGISampleVolumeHierarchy(
    Moer::LightingData lighting_data,
    Moer::ProbeVolumeGpuDesc volume,
    float3 world_pos,
    float3 normal,
    out float resident_coverage,
    out float resolved_level,
    out float transition_weight
) {
    resident_coverage = 0.0;
    resolved_level = 0.0;
    transition_weight = 0.0;
    const float3 biased_pos = world_pos + normal * volume.origin_bias.w;
    if (!ProbeGIIsInsideVolume(volume, biased_pos)) {
        return float3(0.0, 0.0, 0.0);
    }

    uint desired_level;
    uint transition_level;
    ProbeGIResolveAdaptiveLevels(
        lighting_data,
        volume,
        biased_pos,
        desired_level,
        transition_level,
        transition_weight
    );

    float primary_coverage = 0.0;
    float primary_resolved_level = float(desired_level);
    const float3 primary_irradiance = ProbeGISampleHierarchyFromLevel(
        lighting_data,
        volume,
        world_pos,
        normal,
        desired_level,
        primary_coverage,
        primary_resolved_level
    );
    if (transition_level <= desired_level || transition_weight <= 1e-5) {
        resident_coverage = primary_coverage;
        resolved_level = primary_resolved_level;
        transition_weight = 0.0;
        return primary_irradiance;
    }

    float transition_coverage = 0.0;
    float transition_resolved_level = float(transition_level);
    const float3 transition_irradiance = ProbeGISampleHierarchyFromLevel(
        lighting_data,
        volume,
        world_pos,
        normal,
        transition_level,
        transition_coverage,
        transition_resolved_level
    );
    const float primary_weight = primary_coverage * (1.0 - transition_weight);
    const float coarse_weight = transition_coverage * transition_weight;
    const float combined_weight = primary_weight + coarse_weight;
    if (combined_weight <= 1e-5) {
        transition_weight = 0.0;
        return float3(0.0, 0.0, 0.0);
    }

    resident_coverage = saturate(combined_weight);
    resolved_level =
        (primary_resolved_level * primary_weight + transition_resolved_level * coarse_weight) /
        combined_weight;
    return (primary_irradiance * primary_weight + transition_irradiance * coarse_weight) /
           combined_weight;
}

float3 ProbeGISampleVolumeIrradiance(
    Moer::LightingData lighting_data,
    Moer::ProbeVolumeGpuDesc volume,
    float3 world_pos,
    float3 normal,
    out float resident_coverage
) {
    float resolved_level;
    float transition_weight;
    return ProbeGISampleVolumeHierarchy(
        lighting_data,
        volume,
        world_pos,
        normal,
        resident_coverage,
        resolved_level,
        transition_weight
    );
}

float3 ProbeGISampleIrradiance(Moer::LightingData lighting_data, float3 world_pos, float3 normal) {
    if (!ProbeGIIsEnabled(lighting_data)) {
        return float3(0.0, 0.0, 0.0);
    }

    float3 irradiance_sum = float3(0.0, 0.0, 0.0);
    float  volume_weight_sum = 0.0;

    [unroll] for (uint volume_index = 0u; volume_index < Moer::RASTER_PROBE_VOLUME_MAX_COUNT; ++volume_index) {
        if (volume_index >= lighting_data.probe_system_counts.x) {
            continue;
        }

        const Moer::ProbeVolumeGpuDesc volume = ProbeGILoadVolume(lighting_data, volume_index);
        const float3 biased_pos = world_pos + normal * volume.origin_bias.w;
        if (!ProbeGIIsInsideVolume(volume, biased_pos)) {
            continue;
        }

        float resident_coverage = 0.0;
        const float3 volume_irradiance =
            ProbeGISampleVolumeIrradiance(lighting_data, volume, world_pos, normal, resident_coverage);
        if (resident_coverage <= 1e-5) {
            continue;
        }

        const float volume_weight = ProbeGIGetVolumeBlendWeight(volume, biased_pos) * resident_coverage;
        irradiance_sum += volume_irradiance * max(volume.spacing_intensity.w, 0.0) * volume_weight;
        volume_weight_sum += volume_weight;
    }

    return volume_weight_sum > 1e-5 ? irradiance_sum / volume_weight_sum : float3(0.0, 0.0, 0.0);
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
    return irradiance * surface_tint * diffuse_weight * normal_weight * shadow_fill;
}

bool ProbeGISelectVolume(
    Moer::LightingData lighting_data,
    float3 world_pos,
    float3 normal,
    out Moer::ProbeVolumeGpuDesc selected_volume,
    out uint selected_volume_index,
    out float3 selected_biased_pos
) {
    selected_volume = (Moer::ProbeVolumeGpuDesc)0;
    selected_volume_index = 0u;
    selected_biased_pos = world_pos;
    float best_weight = -1.0;

    [unroll] for (uint volume_index = 0u; volume_index < Moer::RASTER_PROBE_VOLUME_MAX_COUNT; ++volume_index) {
        if (volume_index >= lighting_data.probe_system_counts.x) {
            continue;
        }

        const Moer::ProbeVolumeGpuDesc volume = ProbeGILoadVolume(lighting_data, volume_index);
        const float3 biased_pos = world_pos + normal * volume.origin_bias.w;
        if (!ProbeGIIsInsideVolume(volume, biased_pos)) {
            continue;
        }

        const float weight = ProbeGIGetVolumeBlendWeight(volume, biased_pos);
        if (weight > best_weight) {
            best_weight = weight;
            selected_volume = volume;
            selected_volume_index = volume_index;
            selected_biased_pos = biased_pos;
        }
    }

    return best_weight >= 0.0;
}

float3 ProbeGIGetVolumeDebugTint(uint volume_index) {
    if (volume_index == 0u) {
        return float3(0.10, 0.85, 1.00);
    }
    if (volume_index == 1u) {
        return float3(1.00, 0.62, 0.10);
    }
    if (volume_index == 2u) {
        return float3(0.35, 1.00, 0.28);
    }
    return float3(0.95, 0.25, 0.82);
}

float3 ProbeGIGetDebugColor(Moer::LightingData lighting_data, float3 world_pos, float3 normal) {
    if (!ProbeGIIsEnabled(lighting_data)) {
        return float3(0.0, 0.0, 0.0);
    }

    const uint debug_mode = lighting_data.probe_system_config.y;
    if (debug_mode == 2u) {
        return ProbeGISampleIrradiance(lighting_data, world_pos, normal) * lighting_data.probe_system_debug.x;
    }

    Moer::ProbeVolumeGpuDesc volume;
    uint selected_volume_index;
    float3 biased_pos;
    if (!ProbeGISelectVolume(lighting_data, world_pos, normal, volume, selected_volume_index, biased_pos)) {
        return float3(0.02, 0.02, 0.02);
    }

    if (debug_mode == 4u || debug_mode == 5u || debug_mode == 6u || debug_mode == 7u || debug_mode == 8u ||
        debug_mode == 9u || debug_mode == 10u || debug_mode == 11u) {
        uint3 counts = ProbeGIGetCounts(volume);
        float3 local = ProbeGIGetLocalCoord(volume, biased_pos);
        uint3 coord = min(uint3(round(clamp(local, float3(0.0, 0.0, 0.0), float3(counts - uint3(1, 1, 1))))), counts - uint3(1, 1, 1));
        uint brick_index = ProbeGIGetBrickIndex(lighting_data, volume, coord);
        if (debug_mode == 5u) {
            if (brick_index == Moer::RASTER_PROBE_PAGE_INVALID) {
                return float3(0.90, 0.03, 0.02) * lighting_data.probe_system_debug.x;
            }

            const float brick_tint = frac(float(brick_index) * 0.61803398875);
            return lerp(float3(0.05, 0.80, 0.35), float3(0.05, 0.65, 1.00), brick_tint) *
                   lighting_data.probe_system_debug.x;
        }

        if (debug_mode == 6u) {
            if (brick_index == Moer::RASTER_PROBE_PAGE_INVALID) {
                return float3(0.12, 0.01, 0.10) * lighting_data.probe_system_debug.x;
            }

            ArrayBuffer brick_buffer = ArrayBuffer(lighting_data.probe_system_counts.z);
            const Moer::ProbeBrickGpuDesc brick =
                brick_buffer.Load<Moer::ProbeBrickGpuDesc>(brick_index);
            const float age01 = saturate(float(brick.local_counts.w) / 16.0);
            const float3 fresh_color = float3(0.02, 0.85, 0.60);
            const float3 middle_color = float3(1.00, 0.82, 0.06);
            const float3 stale_color = float3(0.95, 0.04, 0.02);
            const float3 age_color = age01 < 0.5 ?
                                         lerp(fresh_color, middle_color, age01 * 2.0) :
                                         lerp(middle_color, stale_color, (age01 - 0.5) * 2.0);
            return age_color * lighting_data.probe_system_debug.x;
        }

        if (debug_mode == 7u) {
            if (brick_index == Moer::RASTER_PROBE_PAGE_INVALID) {
                return float3(0.04, 0.01, 0.01) * lighting_data.probe_system_debug.x;
            }

            ArrayBuffer brick_buffer = ArrayBuffer(lighting_data.probe_system_counts.z);
            const Moer::ProbeBrickGpuDesc brick =
                brick_buffer.Load<Moer::ProbeBrickGpuDesc>(brick_index);
            const float allocation_tint = frac(float(brick.probe_range.x) * 0.61803398875);
            const float3 physical_color = 0.55 + 0.45 * cos(
                6.28318530718 * (allocation_tint + float3(0.00, 0.33, 0.67))
            );
            return physical_color * lighting_data.probe_system_debug.x;
        }

        if (debug_mode == 8u) {
            if (lighting_data.probe_system_hierarchy.x == 0u) {
                return float3(0.03, 0.01, 0.04) * lighting_data.probe_system_debug.x;
            }

            const uint cell_index = ProbeGIGetCellIndex(volume, coord);
            if (cell_index == Moer::RASTER_PROBE_PAGE_INVALID ||
                cell_index >= lighting_data.probe_system_hierarchy.y) {
                return float3(0.35, 0.0, 0.35) * lighting_data.probe_system_debug.x;
            }

            ArrayBuffer cell_buffer = ArrayBuffer(lighting_data.probe_system_hierarchy.x);
            const Moer::ProbeCellGpuDesc cell = cell_buffer.Load<Moer::ProbeCellGpuDesc>(cell_index);
            const float cell_tint = frac(float(
                cell.coord_volume.x + cell.coord_volume.y * 7u + cell.coord_volume.z * 37u +
                cell.coord_volume.w * 101u
            ) * 0.61803398875);
            const float3 cell_color = 0.55 + 0.45 * cos(
                6.28318530718 * (cell_tint + float3(0.00, 0.33, 0.67))
            );
            return cell_color * lighting_data.probe_system_debug.x;
        }

        if (debug_mode == 9u) {
            if (lighting_data.probe_system_hierarchy.x == 0u) {
                return float3(0.03, 0.01, 0.04) * lighting_data.probe_system_debug.x;
            }

            const uint cell_index = ProbeGIGetCellIndex(volume, coord);
            if (cell_index == Moer::RASTER_PROBE_PAGE_INVALID ||
                cell_index >= lighting_data.probe_system_hierarchy.y) {
                return float3(0.35, 0.0, 0.35) * lighting_data.probe_system_debug.x;
            }

            ArrayBuffer cell_buffer = ArrayBuffer(lighting_data.probe_system_hierarchy.x);
            const Moer::ProbeCellGpuDesc cell = cell_buffer.Load<Moer::ProbeCellGpuDesc>(cell_index);
            const float3 fine_color = float3(0.95, 0.10, 0.04);
            const float3 medium_color = float3(1.00, 0.72, 0.05);
            const float3 coarse_color = float3(0.04, 0.32, 1.00);
            const uint desired_level = min(cell.geometry.z, Moer::RASTER_PROBE_MAX_SUBDIVISION_LEVEL);
            const float3 level_color = desired_level == 0u ?
                                           fine_color :
                                           (desired_level == 1u ? medium_color : coarse_color);
            const float occupancy_weight = 0.45 + 0.55 * sqrt(saturate(cell.extent.w));
            return level_color * occupancy_weight * lighting_data.probe_system_debug.x;
        }

        if (debug_mode == 10u) {
            float resident_coverage;
            float resolved_level;
            float transition_weight;
            ProbeGISampleVolumeHierarchy(
                lighting_data,
                volume,
                world_pos,
                normal,
                resident_coverage,
                resolved_level,
                transition_weight
            );
            if (resident_coverage <= 1e-5) {
                return float3(0.85, 0.01, 0.02) * lighting_data.probe_system_debug.x;
            }

            const float3 fine_color = float3(0.04, 0.90, 0.30);
            const float3 medium_color = float3(1.00, 0.72, 0.04);
            const float3 coarse_color = float3(0.04, 0.34, 1.00);
            const float3 resolve_color = resolved_level < 1.0 ?
                                             lerp(fine_color, medium_color, saturate(resolved_level)) :
                                             lerp(medium_color, coarse_color, saturate(resolved_level - 1.0));
            const float3 transition_color = lerp(
                resolve_color,
                float3(1.0, 1.0, 1.0),
                transition_weight * 0.18
            );
            return transition_color * (0.35 + 0.65 * resident_coverage) *
                   lighting_data.probe_system_debug.x;
        }

        if (debug_mode == 11u) {
            if (lighting_data.probe_system_hierarchy.x != 0u) {
                const uint cell_index = ProbeGIGetCellIndex(volume, coord);
                if (cell_index != Moer::RASTER_PROBE_PAGE_INVALID &&
                    cell_index < lighting_data.probe_system_hierarchy.y) {
                    ArrayBuffer cell_buffer = ArrayBuffer(lighting_data.probe_system_hierarchy.x);
                    const Moer::ProbeCellGpuDesc cell =
                        cell_buffer.Load<Moer::ProbeCellGpuDesc>(cell_index);
                    const uint start_level =
                        min(cell.hierarchy.x, Moer::RASTER_PROBE_MAX_SUBDIVISION_LEVEL);
                    const uint end_level =
                        min(cell.hierarchy.y, Moer::RASTER_PROBE_MAX_SUBDIVISION_LEVEL);
                    brick_index = Moer::RASTER_PROBE_PAGE_INVALID;
                    for (uint level = start_level; level <= end_level; ++level) {
                        const uint candidate =
                            ProbeGIGetBrickIndexAtLevel(lighting_data, volume, coord, level);
                        if (candidate != Moer::RASTER_PROBE_PAGE_INVALID) {
                            brick_index = candidate;
                            break;
                        }
                    }
                }
            }

            if (brick_index == Moer::RASTER_PROBE_PAGE_INVALID) {
                return float3(0.18, 0.01, 0.02) * lighting_data.probe_system_debug.x;
            }

            ArrayBuffer brick_buffer = ArrayBuffer(lighting_data.probe_system_counts.z);
            const Moer::ProbeBrickGpuDesc brick =
                brick_buffer.Load<Moer::ProbeBrickGpuDesc>(brick_index);
            const uint dirty_flags = brick.neighbor_pages_1.w;
            const uint dirty_reasons = dirty_flags & Moer::RASTER_PROBE_DIRTY_REASON_MASK;
            if (dirty_reasons == 0u) {
                return float3(0.035, 0.075, 0.055) * lighting_data.probe_system_debug.x;
            }

            float3 dirty_color = float3(0.0, 0.0, 0.0);
            float  reason_count = 0.0;
            if ((dirty_reasons & Moer::RASTER_PROBE_DIRTY_GEOMETRY) != 0u) {
                dirty_color += float3(1.00, 0.28, 0.03);
                reason_count += 1.0;
            }
            if ((dirty_reasons & Moer::RASTER_PROBE_DIRTY_DYNAMIC) != 0u) {
                dirty_color += float3(0.92, 0.05, 0.78);
                reason_count += 1.0;
            }
            if ((dirty_reasons & Moer::RASTER_PROBE_DIRTY_LIGHT) != 0u) {
                dirty_color += float3(1.00, 0.88, 0.05);
                reason_count += 1.0;
            }
            if ((dirty_reasons & Moer::RASTER_PROBE_DIRTY_MATERIAL) != 0u) {
                dirty_color += float3(0.04, 0.72, 1.00);
                reason_count += 1.0;
            }
            dirty_color /= max(reason_count, 1.0);
            if ((dirty_flags & Moer::RASTER_PROBE_DIRTY_SCHEDULED) != 0u) {
                dirty_color = lerp(dirty_color, float3(0.05, 0.95, 0.35), 0.45);
            }
            return dirty_color * lighting_data.probe_system_debug.x;
        }

        uint probe_index = ProbeGIGetProbeIndex(lighting_data, volume, coord);
        if (probe_index == Moer::RASTER_PROBE_PAGE_INVALID) {
            return float3(0.30, 0.01, 0.01) * lighting_data.probe_system_debug.x;
        }
        Moer::ProbeGridProbeData probe = ProbeGILoadProbe(lighting_data, probe_index);
        float3 probe_to_surface = biased_pos - probe.world_position.xyz;
        float receiver_distance = length(probe_to_surface);
        float3 probe_to_surface_dir = receiver_distance > 1e-4 ? probe_to_surface / receiver_distance : float3(0.0, 1.0, 0.0);
        float4 directional_visibility =
            ProbeGILoadDirectionalVisibility(lighting_data, probe_index, probe, probe_to_surface_dir);
        float open_ratio = saturate(directional_visibility.z);
        float mean_distance = saturate(
            directional_visibility.x / max(max(volume.extent_blend.x, volume.extent_blend.y), volume.extent_blend.z)
        );
        return float3(1.0 - open_ratio, open_ratio, mean_distance) * lighting_data.probe_system_debug.x;
    }

    float3 local = ProbeGIGetLocalCoord(volume, biased_pos);
    float3 coord01 = saturate((biased_pos - volume.origin_bias.xyz) / volume.extent_blend.xyz);
    float3 cell_frac = abs(frac(local) - 0.5);
    float grid_line = 1.0 - smoothstep(0.42, 0.49, max(max(cell_frac.x, cell_frac.y), cell_frac.z));
    float3 volume_tint = ProbeGIGetVolumeDebugTint(selected_volume_index);
    float3 volume_color = lerp(coord01 * 0.25 + volume_tint * 0.20, volume_tint, grid_line);
    return volume_color * lighting_data.probe_system_debug.x;
}

#endif // MOER_RASTER_DEFERRED_LIGHTING_PROBE_GI_HLSLI
