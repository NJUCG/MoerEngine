#include "core/common/Bindless.hlsl"
#include "core/common/Common.hlsl"

BINDLESS_BINDINGS(3, 2, 4, 5);

#include "core/math/Math.hlsli"
#include "core/math/STL.hlsli"
#include "materials/Material.hlsli"
#include "shared/Geometry.h"
#include "shared/ShaderParameters.h"
#include "shared/raster/SharedEnum.h"
#include "shared/raster/ShaderParameters.h"
#include "shared/scene/SharedSceneStruct.h"
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

[[vk::binding(7, 0)]] StructuredBuffer<Moer::ProbeVolumeGpuDesc> probe_volume_data;
[[vk::binding(8, 0)]] StructuredBuffer<Moer::ProbeBrickGpuDesc> probe_brick_data;

float3 ProbeSafeNormalize(float3 value, float3 fallback) {
    const float len_sq = dot(value, value);
    return len_sq > 1e-6 ? value * rsqrt(len_sq) : fallback;
}

float2 ProbeEncodeOctahedral(float3 direction) {
    float3 n = direction / max(abs(direction.x) + abs(direction.y) + abs(direction.z), 1e-5);
    if (n.z < 0.0) {
        const float2 sign_xy = float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
        n.xy = (1.0 - abs(n.yx)) * sign_xy;
    }
    return saturate(n.xy * 0.5 + 0.5);
}

uint ProbeGetAtlasTexelIndex(float3 direction) {
    const uint dim = Moer::RASTER_PROBE_VISIBILITY_ATLAS_DIM;
    const uint2 texel = min(uint2(ProbeEncodeOctahedral(direction) * float(dim)), uint2(dim - 1u, dim - 1u));
    return texel.y * dim + texel.x;
}

float3 ProbeRotateDirection(float3 direction) {
    const float4 rotation = param.probe_ray_rotation * rsqrt(max(dot(param.probe_ray_rotation, param.probe_ray_rotation), 1e-6));
    const float3 twice_cross = 2.0 * cross(rotation.xyz, direction);
    return ProbeSafeNormalize(
        direction + rotation.w * twice_cross + cross(rotation.xyz, twice_cross),
        direction
    );
}

float3 ProbeGetTraceRayDirection(uint ray_index, uint ray_count) {
    const float count = float(max(ray_count, 1u));
    const float y = 1.0 - 2.0 * (float(ray_index) + 0.5) / count;
    const float radius = sqrt(max(1.0 - y * y, 0.0));
    const float azimuth = 2.0 * PI * frac(float(ray_index) * 0.6180339887498948);
    const float3 base_direction = float3(cos(azimuth) * radius, y, sin(azimuth) * radius);
    return ProbeRotateDirection(base_direction);
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

void ProbeClearAtlasHistory(uint probe_index, bool clear_irradiance, bool clear_visibility) {
    [unroll] for (uint atlas_texel_index = 0u;
                  atlas_texel_index < Moer::RASTER_PROBE_VISIBILITY_ATLAS_TEXEL_COUNT;
                  ++atlas_texel_index) {
        Moer::ProbeGridIrradianceTexel irradiance_texel;
        irradiance_texel.irradiance = float4(0.0, 0.0, 0.0, 0.0);
        Moer::ProbeGridVisibilityTexel visibility_texel;
        visibility_texel.moments = float4(0.0, 0.0, 0.0, 0.0);
        const uint texel_x = atlas_texel_index % Moer::RASTER_PROBE_IRRADIANCE_ATLAS_DIM;
        const uint texel_y = atlas_texel_index / Moer::RASTER_PROBE_IRRADIANCE_ATLAS_DIM;
        if (clear_irradiance) {
            rw_irradiance_atlas[ProbeGetIrradianceAtlasIndex(probe_index, atlas_texel_index)] = irradiance_texel;
            ProbeWriteAtlasTexelWithBorder(
                rw_irradiance_atlas_texture,
                probe_index,
                texel_x,
                texel_y,
                irradiance_texel.irradiance
            );
        }
        if (clear_visibility) {
            rw_visibility_atlas[ProbeGetVisibilityAtlasIndex(probe_index, atlas_texel_index)] = visibility_texel;
            ProbeWriteAtlasTexelWithBorder(
                rw_visibility_atlas_texture,
                probe_index,
                texel_x,
                texel_y,
                visibility_texel.moments
            );
        }
    }
}

bool ProbeMarkAtlasTexelVisited(uint atlas_texel_index, inout uint visited_low, inout uint visited_high) {
    const uint bit = 1u << (atlas_texel_index & 31u);
    if (atlas_texel_index < 32u) {
        const bool visited = (visited_low & bit) != 0u;
        visited_low |= bit;
        return visited;
    }

    const bool visited = (visited_high & bit) != 0u;
    visited_high |= bit;
    return visited;
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

float3 ProbeEstimateMissRadiance(float3 direction) {
    if (param.probe_update_context.z != 0u) {
        const float3 environment_radiance =
            TextureHandle(param.probe_update_context.z).SampleLevelCube<float3>(direction, 0.0);
        return max(environment_radiance * param.probe_sky_color.a, float3(0.0, 0.0, 0.0));
    }

    const float sky_weight = saturate(direction.y * 0.5 + 0.5);
    return max(
        lerp(param.probe_ground_color.rgb, param.probe_sky_color.rgb, sky_weight) * param.probe_sky_color.a,
        float3(0.0, 0.0, 0.0)
    );
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

float ProbeTraceShadow(float3 origin, float3 direction, float max_distance) {
#if PROBE_GI_USE_RAY_QUERY
    if (max_distance <= 1e-4) {
        return 1.0;
    }
    return ProbeTraceRay(origin, direction, 0.0, max_distance).visible;
#else
    return 1.0;
#endif
}

float3 ProbeEvaluateDirectIrradiance(float3 world_position, float3 surface_normal, float trace_bias) {
    if (param.probe_update_context.x == 0u || param.probe_update_context.y == 0u) {
        return float3(0.0, 0.0, 0.0);
    }

    ArrayBuffer light_buffer = ArrayBuffer(param.probe_update_context.x);
    float3 direct_irradiance = float3(0.0, 0.0, 0.0);
    const float surface_offset = max(trace_bias, 0.002);
    const float3 shadow_origin = world_position + surface_normal * surface_offset;

    [loop] for (uint light_index = 0u; light_index < param.probe_update_context.y; ++light_index) {
        const Moer::GLight light = light_buffer.Load<Moer::GLight>(light_index);
        const float3 light_energy = max(light.color * light.intensity, float3(0.0, 0.0, 0.0));

        if (light.type == Moer::ELightType::Directional) {
            const float3 direction_to_light = ProbeSafeNormalize(-light.direction, float3(0.0, 1.0, 0.0));
            const float n_dot_light = saturate(dot(surface_normal, direction_to_light));
            if (n_dot_light <= 0.0) {
                continue;
            }

            const float shadow_distance = max(param.probe_trace_config.x * 64.0, 1024.0);
            const float shadow = ProbeTraceShadow(shadow_origin, direction_to_light, shadow_distance);
            direct_irradiance += light_energy * n_dot_light * shadow;
            continue;
        }

        if (light.type == Moer::ELightType::Point || light.type == Moer::ELightType::Spot) {
            const float3 to_light = light.position - world_position;
            const float distance_sq = dot(to_light, to_light);
            if (distance_sq <= 1e-6) {
                continue;
            }

            const float distance_to_light = sqrt(distance_sq);
            const float3 direction_to_light = to_light / distance_to_light;
            const float n_dot_light = saturate(dot(surface_normal, direction_to_light));
            if (n_dot_light <= 0.0) {
                continue;
            }

            float spot_weight = 1.0;
            if (light.type == Moer::ELightType::Spot) {
                const float cone_cosine = dot(-direction_to_light, ProbeSafeNormalize(light.direction, float3(0.0, -1.0, 0.0)));
                spot_weight = saturate(
                    (cone_cosine - light.info.y) / max(light.info.x - light.info.y, 1e-4)
                );
            }

            const float shadow_distance = max(distance_to_light - surface_offset, 0.0);
            const float shadow = ProbeTraceShadow(shadow_origin, direction_to_light, shadow_distance);
            const float attenuation = rcp(max(4.0 * PI * distance_sq, 1e-4));
            direct_irradiance += light_energy * n_dot_light * attenuation * spot_weight * shadow;
            continue;
        }

        if (light.type == Moer::ELightType::Ambient || light.type == Moer::ELightType::Environment) {
            direct_irradiance += light_energy;
        }
    }

    return max(direct_irradiance, float3(0.0, 0.0, 0.0));
}

float3 ProbeEvaluateHitRadiance(
    ProbeTraceResult trace_result,
    float3 ray_origin,
    float3 ray_direction,
    float trace_bias
) {
#if PROBE_GI_USE_RAY_QUERY
    if (trace_result.instance_id == ~0u) {
        return float3(0.0, 0.0, 0.0);
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

    float3 surface_normal = ProbeSafeNormalize(material.normal, -ray_direction);
    if (dot(surface_normal, ray_direction) > 0.0) {
        surface_normal = -surface_normal;
    }

    const float3 hit_position = ray_origin + ray_direction * trace_result.hit_distance;
    const float3 direct_irradiance =
        ProbeEvaluateDirectIrradiance(hit_position, surface_normal, trace_bias);
    const float3 diffuse_radiance = material.diffuse_albedo * direct_irradiance * rcp(PI);
    return max(material.emissive + diffuse_radiance, float3(0.0, 0.0, 0.0));
#else
    return float3(0.0, 0.0, 0.0);
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

        const float3 ray_direction = ProbeGetTraceRayDirection(atlas_texel_index, trace_texel_count);
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

[numthreads(64, 1, 1)] void main(uint local_probe_index : SV_DispatchThreadID) {
    const uint brick_index = param.probe_update_context.w;
    const Moer::ProbeBrickGpuDesc brick = probe_brick_data[brick_index];
    if (brick.probe_range.z == 0u || brick.coord_volume.w != param.probe_volume_counts.w) {
        return;
    }

    const uint3 counts = max(param.probe_volume_counts.xyz, uint3(1, 1, 1));
    const uint3 local_counts = max(brick.local_counts.xyz, uint3(1, 1, 1));
    const uint local_probe_count = local_counts.x * local_counts.y * local_counts.z;
    if (local_probe_index >= local_probe_count) {
        return;
    }

    const uint  x      = local_probe_index % local_counts.x;
    const uint  yz     = local_probe_index / local_counts.x;
    const uint  y      = yz % local_counts.y;
    const uint  z      = yz / local_counts.y;
    const uint3 local_coord = uint3(x, y, z);
    const uint3 coord = brick.coord_volume.xyz * Moer::RASTER_PROBE_BRICK_DIM + local_coord;
    if (any(coord >= counts)) {
        return;
    }
    const uint probe_index = brick.probe_range.x + local_probe_index;

    const float3 grid_position = param.probe_volume_origin.xyz + param.probe_volume_spacing.xyz * float3(coord);

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
    const bool probe_state_history_valid =
        history_probe.irradiance.a > 0.0 &&
        ProbeDecodeState(history_probe.world_position.w) != Moer::RASTER_PROBE_STATE_INVALID;
    const bool irradiance_history_valid =
        probe_state_history_valid && irradiance_history_weight > 0.0;
    const bool visibility_history_valid =
        probe_state_history_valid && visibility_history_weight > 0.0 && history_probe.visibility.w > 0.0;
    if (!irradiance_history_valid || !visibility_history_valid) {
        ProbeClearAtlasHistory(probe_index, !irradiance_history_valid, !visibility_history_valid);
    }

    float  distance_sum = 0.0;
    float  distance_sq_sum = 0.0;
    float  open_sum = 0.0;
    float3 ray_radiance_sum = float3(0.0, 0.0, 0.0);
    float  close_hit_sum = 0.0;
    float  very_close_hit_sum = 0.0;
    uint   visited_atlas_low = 0u;
    uint   visited_atlas_high = 0u;
    const float near_distance = ProbeGetNearGeometryDistance(max_trace_distance, trace_bias);
    const float invalid_distance = max(trace_bias * 1.5, near_distance * 0.18);

    [loop] for (uint ray_index = 0u; ray_index < trace_texel_count; ++ray_index) {
        const float3 ray_direction = ProbeGetTraceRayDirection(ray_index, trace_texel_count);
        const float3 ray_origin = position + ray_direction * trace_bias;
        const ProbeTraceResult trace_result =
            ProbeTraceRay(ray_origin, ray_direction, 0.0, max_trace_distance);
        const float hit_distance = trace_result.hit_distance;
        const float visible = trace_result.visible;

        const float hit = 1.0 - visible;
        const float close_hit =
            hit * (1.0 - smoothstep(near_distance, near_distance * 1.75, hit_distance));
        close_hit_sum += close_hit;
        very_close_hit_sum += hit * (hit_distance < invalid_distance ? 1.0 : 0.0);

        const float3 miss_radiance = ProbeEstimateMissRadiance(ray_direction);
        const float3 hit_radiance =
            ProbeEvaluateHitRadiance(trace_result, ray_origin, ray_direction, trace_bias);
        const float3 directional_irradiance = lerp(hit_radiance, miss_radiance, visible);

        ray_radiance_sum += directional_irradiance;
        distance_sum += hit_distance;
        distance_sq_sum += hit_distance * hit_distance;
        open_sum += visible;

        const uint atlas_texel_index = ProbeGetAtlasTexelIndex(ray_direction);
        const bool texel_visited = ProbeMarkAtlasTexelVisited(
            atlas_texel_index,
            visited_atlas_low,
            visited_atlas_high
        );
        const float texel_confidence = trace_blend;
        const uint irradiance_atlas_index = ProbeGetIrradianceAtlasIndex(probe_index, atlas_texel_index);
        const Moer::ProbeGridIrradianceTexel history_irradiance_texel =
            rw_irradiance_atlas[irradiance_atlas_index];
        float3 final_directional_irradiance = directional_irradiance;
        if (texel_visited && history_irradiance_texel.irradiance.w > 0.0) {
            final_directional_irradiance =
                0.5 * (final_directional_irradiance + history_irradiance_texel.irradiance.rgb);
        } else if (irradiance_history_valid && texel_confidence > 0.0) {
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
        const Moer::ProbeGridVisibilityTexel history_texel = rw_visibility_atlas[visibility_atlas_index];
        float4 final_visibility_moments = float4(hit_distance, hit_distance * hit_distance, visible, texel_confidence);
        if (texel_visited && history_texel.moments.w > 0.0) {
            final_visibility_moments.xyz = 0.5 * (final_visibility_moments.xyz + history_texel.moments.xyz);
        } else if (visibility_history_valid && texel_confidence > 0.0) {
            const float history_valid = history_texel.moments.w > 0.0 ? 1.0 : 0.0;
            final_visibility_moments.xyz =
                lerp(final_visibility_moments.xyz, history_texel.moments.xyz, visibility_history_weight * history_valid);
        }

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

    const float inv_ray_count = 1.0 / float(trace_texel_count);
    const float mean_distance = distance_sum * inv_ray_count;
    const float mean_distance_sq = distance_sq_sum * inv_ray_count;
    const float open_ratio = open_sum * inv_ray_count;
    const float current_open_ratio = open_ratio;
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

    const float3 irradiance = max(ray_radiance_sum * inv_ray_count, float3(0.0, 0.0, 0.0));

    float3 final_irradiance = irradiance;
    if (irradiance_history_valid && probe_confidence > 0.0) {
        final_irradiance = lerp(irradiance, history_probe.irradiance.rgb, irradiance_history_weight);
    }

    float3 final_visibility = float3(mean_distance, mean_distance_sq, open_ratio);
    if (visibility_history_valid && probe_confidence > 0.0) {
        final_visibility = lerp(final_visibility, history_probe.visibility.xyz, visibility_history_weight);
    }

    Moer::ProbeGridProbeData probe;
    probe.world_position      = float4(position, float(probe_state));
    probe.irradiance          = float4(final_irradiance, probe_confidence);
    probe.visibility          = float4(final_visibility, trace_blend);
    rw_probe_data[probe_index] = probe;
}
