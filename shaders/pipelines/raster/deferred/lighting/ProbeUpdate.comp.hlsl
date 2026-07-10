#include "core/common/Bindless.hlsl"
#include "core/common/Common.hlsl"

BINDLESS_BINDINGS(3, 2, 4, 5);

#include "core/math/Math.hlsli"
#include "core/math/STL.hlsli"
#include "materials/Material.hlsli"
#include "shared/Geometry.h"
#include "shared/ShaderParameters.h"
#include "shared/raster/ShaderParameters.h"
#include "shared/utils/MoerMath.hlsli"
#include "shared/utils/Packing.h"
#include "pipelines/raytracing/inline/RaytracingCommon.hlsli"

[[vk::push_constant]] ConstantBuffer<Moer::ProbeUpdateParam> param;

[[vk::binding(0, 0)]] RWStructuredBuffer<Moer::ProbeGridProbeData> rw_probe_data;
[[vk::binding(1, 0)]] RWStructuredBuffer<Moer::ProbeGridVisibilityTexel> rw_visibility_atlas;
[[vk::binding(2, 0)]] RWStructuredBuffer<Moer::ProbeGridIrradianceTexel> rw_irradiance_atlas;
[[vk::binding(3, 0)]] StructuredBuffer<Moer::GBufferPassParams> probe_scene_data;
[[vk::binding(4, 0)]] RWTexture2D<float4> rw_visibility_atlas_texture;
[[vk::binding(5, 0)]] RWTexture2D<float4> rw_irradiance_atlas_texture;

#if PROBE_GI_USE_RAY_QUERY
[[vk::binding(6, 0)]] RaytracingAccelerationStructure tlas;
#endif

float ProbeSafeSaturate(float value) {
    return saturate(value);
}

float3 ProbeSafeNormalize(float3 value, float3 fallback) {
    const float len_sq = dot(value, value);
    return len_sq > 1e-6 ? value * rsqrt(len_sq) : fallback;
}

float3 ProbeGetGridCoord01(uint3 coord, uint3 counts) {
    return float3(
        counts.x > 1u ? float(coord.x) / float(counts.x - 1u) : 0.5,
        counts.y > 1u ? float(coord.y) / float(counts.y - 1u) : 0.5,
        counts.z > 1u ? float(coord.z) / float(counts.z - 1u) : 0.5
    );
}

float3 ProbeDecodeOctahedral(float2 encoded) {
    float2 f = encoded * 2.0 - 1.0;
    float3 n = float3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    if (n.z < 0.0) {
        float2 sign_xy = float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
        float2 folded = (1.0 - abs(n.yx)) * sign_xy;
        n = float3(folded.x, folded.y, n.z);
    }
    return normalize(n);
}

float3 ProbeGetAtlasDirection(uint atlas_texel_index) {
    const uint dim = Moer::RASTER_PROBE_VISIBILITY_ATLAS_DIM;
    const uint x = atlas_texel_index % dim;
    const uint y = atlas_texel_index / dim;
    const float2 uv = (float2(x, y) + 0.5) / float(dim);
    return ProbeDecodeOctahedral(uv);
}

uint ProbeDecodeState(float state_value) {
    return uint(round(max(state_value, 0.0)));
}

uint ProbeGetVisibilityAtlasIndex(uint probe_index, uint atlas_texel_index) {
    return probe_index * Moer::RASTER_PROBE_VISIBILITY_ATLAS_TEXEL_COUNT + atlas_texel_index;
}

uint ProbeGetIrradianceAtlasIndex(uint probe_index, uint atlas_texel_index) {
    return probe_index * Moer::RASTER_PROBE_IRRADIANCE_ATLAS_TEXEL_COUNT + atlas_texel_index;
}

uint2 ProbeGetAtlasTextureTileOrigin(uint probe_index) {
    const uint tile_x = probe_index % Moer::RASTER_PROBE_ATLAS_TILE_COLUMNS;
    const uint tile_y = probe_index / Moer::RASTER_PROBE_ATLAS_TILE_COLUMNS;
    return uint2(tile_x, tile_y) * Moer::RASTER_PROBE_ATLAS_TILE_STRIDE;
}

uint2 ProbeGetAtlasTextureInteriorCoord(uint probe_index, uint texel_x, uint texel_y) {
    return ProbeGetAtlasTextureTileOrigin(probe_index) +
           uint2(texel_x + Moer::RASTER_PROBE_ATLAS_TILE_BORDER, texel_y + Moer::RASTER_PROBE_ATLAS_TILE_BORDER);
}

void ProbeWriteAtlasTexelWithBorder(
    RWTexture2D<float4> atlas_texture,
    uint probe_index,
    uint texel_x,
    uint texel_y,
    float4 value
) {
    const uint dim = Moer::RASTER_PROBE_IRRADIANCE_ATLAS_DIM;
    const uint max_texel = dim - 1u;
    const uint stride = Moer::RASTER_PROBE_ATLAS_TILE_STRIDE;
    const uint border = Moer::RASTER_PROBE_ATLAS_TILE_BORDER;
    const uint2 tile_origin = ProbeGetAtlasTextureTileOrigin(probe_index);

    atlas_texture[ProbeGetAtlasTextureInteriorCoord(probe_index, texel_x, texel_y)] = value;

    if (texel_x == 0u) {
        atlas_texture[tile_origin + uint2(stride - 1u, border + max_texel - texel_y)] = value;
    }
    if (texel_x == max_texel) {
        atlas_texture[tile_origin + uint2(0u, border + max_texel - texel_y)] = value;
    }
    if (texel_y == 0u) {
        atlas_texture[tile_origin + uint2(border + max_texel - texel_x, stride - 1u)] = value;
    }
    if (texel_y == max_texel) {
        atlas_texture[tile_origin + uint2(border + max_texel - texel_x, 0u)] = value;
    }

    if (texel_x == 0u && texel_y == 0u) {
        atlas_texture[tile_origin + uint2(0u, 0u)] = value;
    }
    if (texel_x == max_texel && texel_y == 0u) {
        atlas_texture[tile_origin + uint2(stride - 1u, 0u)] = value;
    }
    if (texel_x == 0u && texel_y == max_texel) {
        atlas_texture[tile_origin + uint2(0u, stride - 1u)] = value;
    }
    if (texel_x == max_texel && texel_y == max_texel) {
        atlas_texture[tile_origin + uint2(stride - 1u, stride - 1u)] = value;
    }
}

float ProbeGetMinSpacing() {
    return max(min(min(param.probe_volume_spacing.x, param.probe_volume_spacing.y), param.probe_volume_spacing.z), 0.05);
}

float ProbeGetNearGeometryDistance(float max_trace_distance, float trace_bias) {
    const float spacing_based = ProbeGetMinSpacing() * 0.35;
    const float lower_bound = max(trace_bias * 2.0, 0.04);
    const float upper_bound = max(max_trace_distance * 0.25, lower_bound);
    return clamp(spacing_based, lower_bound, upper_bound);
}

float3 ProbeEstimateMissRadiance(float3 direction, float3 sun_bounce) {
    const float sky_weight = saturate(direction.y * 0.5 + 0.5);
    const float3 sky_ground =
        lerp(param.probe_ground_color.rgb * 0.55, param.probe_sky_color.rgb, sky_weight) * param.probe_sky_color.a;
    const float sun_alignment = pow(saturate(dot(direction, -param.main_light_direction.xyz)), 48.0);
    return sky_ground + sun_bounce * sun_alignment;
}

struct ProbeTraceResult {
    float  hit_distance;
    float  visible;
    uint   instance_id;
    uint   geometry_index;
    uint   primitive_index;
    uint   backface;
    float2 barycentrics;
};

ProbeTraceResult ProbeMakeMissTrace(float tmax) {
    ProbeTraceResult result;
    result.hit_distance   = tmax;
    result.visible        = 1.0;
    result.instance_id    = ~0u;
    result.geometry_index = 0u;
    result.primitive_index = 0u;
    result.backface       = 0u;
    result.barycentrics   = float2(0.0, 0.0);
    return result;
}

#if PROBE_GI_USE_RAY_QUERY
ProbeTraceResult ProbeTraceRay(float3 origin, float3 direction, float tmin, float tmax) {
    RayDesc ray_desc;
    ray_desc.Origin = origin;
    ray_desc.Direction = direction;
    ray_desc.TMin = tmin;
    ray_desc.TMax = tmax;

    RayQuery<
        RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
        RAY_FLAG_FORCE_OPAQUE>
        ray_query;
    ray_query.TraceRayInline(tlas, RAY_FLAG_NONE, Moer::RTVM_ALL, ray_desc);
    ray_query.Proceed();

    ProbeTraceResult result = ProbeMakeMissTrace(tmax);
    if (ray_query.CommittedStatus() == COMMITTED_NOTHING) {
        return result;
    }

    result.hit_distance    = max(ray_query.CommittedRayT(), tmin);
    result.visible         = 0.0;
    result.instance_id     = ray_query.CommittedInstanceID();
    result.geometry_index  = ray_query.CommittedGeometryIndex();
    result.primitive_index = ray_query.CommittedPrimitiveIndex();
    result.backface        = ray_query.CommittedTriangleFrontFace() ? 0u : 1u;
    result.barycentrics    = ray_query.CommittedTriangleBarycentrics();
    return result;
}
#else
ProbeTraceResult ProbeTraceRay(float3 origin, float3 direction, float tmin, float tmax) {
    return ProbeMakeMissTrace(tmax);
}
#endif

float3 ProbeEstimateHitMaterialRadiance(
    ProbeTraceResult trace_result,
    float3 ray_direction,
    float normalized_hit_distance,
    float3 fallback_hit_bounce,
    float direct_light_energy
) {
#if PROBE_GI_USE_RAY_QUERY
    if (trace_result.instance_id == ~0u) {
        return fallback_hit_bounce;
    }

    const Moer::GBufferPassParams scene_params = probe_scene_data[0];
    Moer::GeometryRecord geom = Moer::GetGeometryRecordFrom(
        scene_params,
        trace_result.instance_id,
        trace_result.geometry_index,
        trace_result.primitive_index,
        trace_result.barycentrics,
        Moer::EGA_UV | Moer::EGA_Normal | Moer::EGA_Tangent,
        trace_result.backface != 0u
    );
    Moer::MaterialSample material = Moer::SampleGeometryMaterial(
        geom,
        float2(0.0, 0.0),
        float2(0.0, 0.0),
        2.0,
        Moer::EMA_BaseColor | Moer::EMA_Normal | Moer::EMA_MetalRough | Moer::EMA_Emissive
    );

    const float3 surface_normal = ProbeSafeNormalize(material.normal, -ray_direction);
    const float facing_probe = saturate(dot(surface_normal, -ray_direction));
    const float n_dot_light = saturate(dot(surface_normal, -param.main_light_direction.xyz));
    const float diffuse_energy = 1.0 - saturate(material.metalness) * 0.85;
    const float rough_bounce = 0.35 + 0.65 * saturate(material.roughness);
    const float distance_energy = 0.30 + 0.70 * saturate(normalized_hit_distance);

    const float3 direct_bounce =
        param.main_light_color.rgb * direct_light_energy * param.probe_ground_color.w * n_dot_light;
    const float3 indirect_bounce = fallback_hit_bounce * (0.35 + 0.65 * facing_probe);
    const float3 material_bounce =
        material.diffuse_albedo * (direct_bounce + indirect_bounce) * diffuse_energy * rough_bounce * distance_energy +
        material.emissive * 0.35;

    return max(lerp(fallback_hit_bounce, material_bounce, 0.85), float3(0.0, 0.0, 0.0));
#else
    return fallback_hit_bounce;
#endif
}

struct ProbePlacementResult {
    float3 position;
    uint   state;
};

ProbePlacementResult ProbeClassifyAndRelocate(
    float3 grid_position,
    uint trace_texel_count,
    float max_trace_distance,
    float trace_bias
) {
    ProbePlacementResult result;
    result.position = grid_position;
    result.state = Moer::RASTER_PROBE_STATE_ACTIVE;

#if PROBE_GI_USE_RAY_QUERY
    if (param.probe_trace_config.w <= 0.0 || trace_texel_count == 0u) {
        return result;
    }

    const float near_distance = ProbeGetNearGeometryDistance(max_trace_distance, trace_bias);
    const float invalid_distance = max(trace_bias * 1.5, near_distance * 0.18);

    float close_hit_sum = 0.0;
    float very_close_hit_sum = 0.0;
    float3 escape_dir_sum = float3(0.0, 0.0, 0.0);

    [loop] for (uint atlas_texel_index = 0u;
                atlas_texel_index < Moer::RASTER_PROBE_VISIBILITY_ATLAS_TEXEL_COUNT;
                ++atlas_texel_index) {
        if (atlas_texel_index >= trace_texel_count) {
            continue;
        }

        const float3 ray_direction = ProbeGetAtlasDirection(atlas_texel_index);
        const ProbeTraceResult trace_result =
            ProbeTraceRay(grid_position, ray_direction, 0.001, max_trace_distance);
        const float hit_distance = trace_result.hit_distance;
        const float visible = trace_result.visible;

        const float hit = 1.0 - visible;
        const float close_hit =
            hit * (1.0 - smoothstep(near_distance, near_distance * 1.75, hit_distance));
        close_hit_sum += close_hit;
        very_close_hit_sum += hit * (hit_distance < invalid_distance ? 1.0 : 0.0);

        const float open_distance_weight = lerp(saturate(hit_distance / near_distance), 1.0, visible);
        escape_dir_sum += ray_direction * open_distance_weight;
        escape_dir_sum -= ray_direction * close_hit * 1.25;
    }

    const float inv_trace_count = 1.0 / float(trace_texel_count);
    const float close_hit_ratio = close_hit_sum * inv_trace_count;
    const float very_close_hit_ratio = very_close_hit_sum * inv_trace_count;
    const float escape_len = length(escape_dir_sum) * inv_trace_count;

    if (close_hit_ratio <= 0.08 && very_close_hit_ratio <= 0.04) {
        return result;
    }

    if ((close_hit_ratio > 0.88 && escape_len < 0.08) || very_close_hit_ratio > 0.70) {
        result.state = Moer::RASTER_PROBE_STATE_INVALID;
        return result;
    }

    const float3 escape_dir = ProbeSafeNormalize(escape_dir_sum, float3(0.0, 1.0, 0.0));
    const float relocation_strength = saturate((close_hit_ratio - 0.04) / 0.58);
    const float relocation_distance = min(near_distance * relocation_strength, ProbeGetMinSpacing() * 0.45);
    result.position = grid_position + escape_dir * relocation_distance;
    result.state =
        relocation_distance > trace_bias * 0.5 ? Moer::RASTER_PROBE_STATE_RELOCATED : Moer::RASTER_PROBE_STATE_NEAR_SURFACE;
#endif

    return result;
}

[numthreads(64, 1, 1)] void main(uint probe_index : SV_DispatchThreadID) {
    const uint total_probe_count = param.probe_volume_counts.w;
    if (probe_index >= total_probe_count) {
        return;
    }

    const uint3 counts = max(param.probe_volume_counts.xyz, uint3(1, 1, 1));
    const uint  x      = probe_index % counts.x;
    const uint  yz     = probe_index / counts.x;
    const uint  y      = yz % counts.y;
    const uint  z      = yz / counts.y;
    const uint3 coord  = uint3(x, y, z);

    const float3 coord01  = ProbeGetGridCoord01(coord, counts);
    const float3 grid_position = param.probe_volume_origin.xyz + param.probe_volume_spacing.xyz * float3(coord);

    const float3 up        = float3(0.0, 1.0, 0.0);
    const float  sun_lift  = ProbeSafeSaturate(dot(-param.main_light_direction.xyz, up));
    const float  direct_light_intensity = max(param.main_light_color.a, 0.0);
    const float  direct_light_energy    = direct_light_intensity / (1.0 + direct_light_intensity);
    const float3 sun_bounce =
        param.main_light_color.rgb * direct_light_energy * param.probe_ground_color.w * (0.35 + 0.65 * sun_lift);

    const float lateral_variation =
        0.88 + 0.12 * sin((coord01.x * 3.17 + coord01.z * 2.41 + param.main_light_direction.w) * PI);
    const float low_volume_weight = pow(saturate(1.0 - coord01.y), 1.35);
    const float wall_bounce_mask = saturate(abs(coord01.x - 0.5) * 2.0) * saturate(1.0 - coord01.y * 0.75);

    const float3 sky_gradient =
        lerp(param.probe_ground_color.rgb * 0.70, param.probe_sky_color.rgb * 0.85, coord01.y) * param.probe_sky_color.a;
    const float3 directional_bounce = sun_bounce * (0.25 + 0.55 * low_volume_weight);
    const float3 local_color_bounce =
        lerp(float3(0.65, 0.26, 0.17), float3(0.12, 0.42, 0.72), smoothstep(0.0, 1.0, coord01.x)) *
        wall_bounce_mask * param.probe_sky_color.a * 0.16;
    const float3 ground_bounce = param.probe_ground_color.rgb * low_volume_weight * param.probe_sky_color.a * 0.30;

    const float max_trace_distance = max(param.probe_trace_config.x, 0.1);
    const float trace_bias = max(param.probe_trace_config.y, 0.02);
    const uint trace_texel_count =
        clamp(uint(param.probe_trace_config.z), 1u, Moer::RASTER_PROBE_VISIBILITY_ATLAS_TEXEL_COUNT);
    const float trace_blend = saturate(param.probe_trace_config.w);

    ProbePlacementResult placement =
        ProbeClassifyAndRelocate(grid_position, trace_texel_count, max_trace_distance, trace_bias);
    const float3 volume_min = param.probe_volume_origin.xyz;
    const float3 volume_max = param.probe_volume_origin.xyz + param.probe_volume_spacing.xyz * float3(counts - uint3(1, 1, 1));
    const float3 position = clamp(placement.position, volume_min, volume_max);
    const float irradiance_history_weight = saturate(param.probe_volume_origin.w);
    const float visibility_history_weight = saturate(param.probe_volume_spacing.w);
    const Moer::ProbeGridProbeData history_probe = rw_probe_data[probe_index];

    float  distance_sum = 0.0;
    float  distance_sq_sum = 0.0;
    float  open_sum = 0.0;
    float  current_open_sum = 0.0;
    float3 ray_radiance_sum = float3(0.0, 0.0, 0.0);
    float  close_hit_sum = 0.0;
    float  very_close_hit_sum = 0.0;
    const float near_distance = ProbeGetNearGeometryDistance(max_trace_distance, trace_bias);
    const float invalid_distance = max(trace_bias * 1.5, near_distance * 0.18);

    [loop] for (uint atlas_texel_index = 0u;
                atlas_texel_index < Moer::RASTER_PROBE_VISIBILITY_ATLAS_TEXEL_COUNT;
                ++atlas_texel_index) {
        const float3 ray_direction = ProbeGetAtlasDirection(atlas_texel_index);
        ProbeTraceResult trace_result = ProbeMakeMissTrace(max_trace_distance);
        if (atlas_texel_index < trace_texel_count) {
            trace_result = ProbeTraceRay(
                position + ray_direction * trace_bias,
                ray_direction,
                0.0,
                max_trace_distance
            );
        }
        const float hit_distance = trace_result.hit_distance;
        const float visible = trace_result.visible;

        const float hit = 1.0 - visible;
        const float close_hit =
            hit * (1.0 - smoothstep(near_distance, near_distance * 1.75, hit_distance));
        if (atlas_texel_index < trace_texel_count) {
            close_hit_sum += close_hit;
            very_close_hit_sum += hit * (hit_distance < invalid_distance ? 1.0 : 0.0);
        }

        const float normalized_hit = saturate(hit_distance / max_trace_distance);
        const float3 miss_radiance = ProbeEstimateMissRadiance(ray_direction, sun_bounce);
        const float3 hit_bounce = (ground_bounce * 0.45 + local_color_bounce * 1.25 + directional_bounce * 0.20) *
                                  (0.25 + 0.75 * normalized_hit);
        const float3 material_hit_bounce =
            ProbeEstimateHitMaterialRadiance(trace_result, ray_direction, normalized_hit, hit_bounce, direct_light_energy);
        const float3 directional_irradiance = lerp(material_hit_bounce, miss_radiance, visible);

        ray_radiance_sum += directional_irradiance;
        current_open_sum += visible;

        const float texel_confidence =
            (atlas_texel_index < trace_texel_count) ? saturate(param.probe_trace_config.w) : 0.0;

        const uint irradiance_atlas_index = ProbeGetIrradianceAtlasIndex(probe_index, atlas_texel_index);
        float3 final_directional_irradiance = directional_irradiance;
        if (irradiance_history_weight > 0.0 && texel_confidence > 0.0) {
            const Moer::ProbeGridIrradianceTexel history_irradiance_texel = rw_irradiance_atlas[irradiance_atlas_index];
            const float history_valid = history_irradiance_texel.irradiance.w > 0.0 ? 1.0 : 0.0;
            final_directional_irradiance = lerp(
                final_directional_irradiance,
                history_irradiance_texel.irradiance.rgb,
                irradiance_history_weight * history_valid
            );
        }

        Moer::ProbeGridIrradianceTexel irradiance_texel;
        irradiance_texel.irradiance = float4(final_directional_irradiance, texel_confidence);
        rw_irradiance_atlas[irradiance_atlas_index] = irradiance_texel;

        const uint visibility_atlas_index = ProbeGetVisibilityAtlasIndex(probe_index, atlas_texel_index);
        float4 final_visibility_moments = float4(hit_distance, hit_distance * hit_distance, visible, texel_confidence);
        if (visibility_history_weight > 0.0 && texel_confidence > 0.0) {
            const Moer::ProbeGridVisibilityTexel history_texel = rw_visibility_atlas[visibility_atlas_index];
            const float history_valid = history_texel.moments.w > 0.0 ? 1.0 : 0.0;
            final_visibility_moments.xyz =
                lerp(final_visibility_moments.xyz, history_texel.moments.xyz, visibility_history_weight * history_valid);
        }

        distance_sum += final_visibility_moments.x;
        distance_sq_sum += final_visibility_moments.y;
        open_sum += final_visibility_moments.z;

        Moer::ProbeGridVisibilityTexel visibility_texel;
        visibility_texel.moments = final_visibility_moments;
        rw_visibility_atlas[visibility_atlas_index] = visibility_texel;

        const uint atlas_texel_x = atlas_texel_index % Moer::RASTER_PROBE_IRRADIANCE_ATLAS_DIM;
        const uint atlas_texel_y = atlas_texel_index / Moer::RASTER_PROBE_IRRADIANCE_ATLAS_DIM;
        ProbeWriteAtlasTexelWithBorder(
            rw_irradiance_atlas_texture,
            probe_index,
            atlas_texel_x,
            atlas_texel_y,
            irradiance_texel.irradiance
        );
        ProbeWriteAtlasTexelWithBorder(
            rw_visibility_atlas_texture,
            probe_index,
            atlas_texel_x,
            atlas_texel_y,
            visibility_texel.moments
        );
    }

    const float inv_ray_count = 1.0 / float(Moer::RASTER_PROBE_VISIBILITY_ATLAS_TEXEL_COUNT);
    const float mean_distance = distance_sum * inv_ray_count;
    const float mean_distance_sq = distance_sq_sum * inv_ray_count;
    const float open_ratio = open_sum * inv_ray_count;
    const float current_open_ratio = current_open_sum * inv_ray_count;
    const float inv_trace_count = 1.0 / float(trace_texel_count);
    const float close_hit_ratio = close_hit_sum * inv_trace_count;
    const float very_close_hit_ratio = very_close_hit_sum * inv_trace_count;

    uint probe_state = placement.state;
    if (trace_blend > 0.0) {
        if (probe_state != Moer::RASTER_PROBE_STATE_INVALID &&
            (very_close_hit_ratio > 0.55 || (close_hit_ratio > 0.88 && current_open_ratio < 0.08))) {
            probe_state = Moer::RASTER_PROBE_STATE_INVALID;
        } else if (probe_state == Moer::RASTER_PROBE_STATE_ACTIVE && close_hit_ratio > 0.16) {
            probe_state = Moer::RASTER_PROBE_STATE_NEAR_SURFACE;
        } else if (probe_state == Moer::RASTER_PROBE_STATE_RELOCATED && close_hit_ratio > 0.50) {
            probe_state = Moer::RASTER_PROBE_STATE_NEAR_SURFACE;
        }
    }

    const float probe_confidence = probe_state == Moer::RASTER_PROBE_STATE_INVALID ? 0.0 : 1.0;

    const float3 fallback_irradiance =
        max((sky_gradient + directional_bounce + ground_bounce + local_color_bounce) * lateral_variation, float3(0.0, 0.0, 0.0));
    const float3 traced_irradiance =
        ray_radiance_sum * inv_ray_count + directional_bounce * 0.25 + local_color_bounce * (1.0 - current_open_ratio) * 0.70;
    const float3 irradiance = lerp(fallback_irradiance, traced_irradiance, trace_blend);

    float3 final_irradiance = irradiance;
    if (irradiance_history_weight > 0.0 && probe_confidence > 0.0 && history_probe.irradiance.a > 0.0 &&
        ProbeDecodeState(history_probe.world_position.w) != Moer::RASTER_PROBE_STATE_INVALID) {
        final_irradiance = lerp(irradiance, history_probe.irradiance.rgb, irradiance_history_weight);
    }

    Moer::ProbeGridProbeData probe;
    probe.world_position      = float4(position, float(probe_state));
    probe.irradiance          = float4(final_irradiance, probe_confidence);
    probe.visibility          = float4(mean_distance, mean_distance_sq, open_ratio, trace_blend);
    rw_probe_data[probe_index] = probe;
}
