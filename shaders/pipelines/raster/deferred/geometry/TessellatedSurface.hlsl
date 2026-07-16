#include "core/common/Common.hlsl"
#include "shared/raster/ShaderParameters.h"

[[vk::binding(0, 0)]] ConstantBuffer<Moer::TessellatedSurfaceData> surface_data;

uint HashCell(int2 cell) {
    uint hash = asuint(cell.x) * 0x8da6b343u;
    hash ^= asuint(cell.y) * 0xd8163841u;
    hash ^= hash >> 16u;
    hash *= 0x7feb352du;
    hash ^= hash >> 15u;
    hash *= 0x846ca68bu;
    return hash ^ (hash >> 16u);
}

float2 Gradient2D(int2 cell) {
    switch (HashCell(cell) & 7u) {
        case 0u: return float2(1.0, 0.0);
        case 1u: return float2(-1.0, 0.0);
        case 2u: return float2(0.0, 1.0);
        case 3u: return float2(0.0, -1.0);
        case 4u: return float2(0.70710678, 0.70710678);
        case 5u: return float2(-0.70710678, 0.70710678);
        case 6u: return float2(0.70710678, -0.70710678);
        default: return float2(-0.70710678, -0.70710678);
    }
}

float Quintic(float value) {
    return value * value * value * (value * (value * 6.0 - 15.0) + 10.0);
}

float Noise2D(float2 position) {
    int2  cell     = int2(floor(position));
    float2 local   = position - float2(cell);
    float2 blended = float2(Quintic(local.x), Quintic(local.y));

    float n00 = dot(Gradient2D(cell + int2(0, 0)), local - float2(0.0, 0.0));
    float n10 = dot(Gradient2D(cell + int2(1, 0)), local - float2(1.0, 0.0));
    float n01 = dot(Gradient2D(cell + int2(0, 1)), local - float2(0.0, 1.0));
    float n11 = dot(Gradient2D(cell + int2(1, 1)), local - float2(1.0, 1.0));

    return lerp(lerp(n00, n10, blended.x), lerp(n01, n11, blended.x), blended.y) * 1.41421356;
}

float Fbm3(float2 position) {
    float value     = 0.0;
    float amplitude = 1.0;
    float weight    = 0.0;

    [unroll]
    for (uint octave = 0u; octave < 3u; ++octave) {
        value += Noise2D(position) * amplitude;
        weight += amplitude;
        position = position * 1.93 + float2(17.17, 9.23);
        amplitude *= 0.48;
    }
    return value / weight;
}

float2 ToWindSpace(float2 world_xz) {
    float2 wind = normalize(surface_data.wind_and_normal.xy);
    float2 cross_wind = float2(-wind.y, wind.x);
    return float2(dot(world_xz, wind), dot(world_xz, cross_wind));
}

float EvaluateSurfaceHeight(float2 world_xz) {
    float2 wind_position = ToWindSpace(world_xz);
    float  macro_noise   = Fbm3(wind_position * surface_data.displacement.z);
    float  height        = 0.0;

    if (surface_data.grid_and_options.z == 0u) {
        float warped_phase =
            wind_position.x * 0.82 + macro_noise * surface_data.wind_and_normal.z;
        float dune_wave = sin(warped_phase) + 0.25 * sin(2.0 * warped_phase + 0.6);
        dune_wave /= 1.25;

        float detail_phase =
            wind_position.x * surface_data.displacement.w +
            0.45 * sin(wind_position.y * 0.31);
        float detail_wave = sin(detail_phase) + 0.18 * sin(2.0 * detail_phase + 1.2);
        detail_wave /= 1.18;

        float shape = 0.42 * macro_noise + 0.58 * dune_wave;
        height = surface_data.displacement.x * (0.68 + 0.52 * shape) +
                 surface_data.displacement.y * detail_wave;
    } else {
        float smooth_detail = Noise2D(
            wind_position * surface_data.displacement.w + float2(23.4, 41.7)
        );
        height = surface_data.displacement.x * (0.72 + 0.36 * macro_noise) +
                 surface_data.displacement.y * smooth_detail;
    }

    float extent = max(surface_data.surface_origin_extent.w, 1e-3);
    float2 normalized_from_center = abs(
        (world_xz - surface_data.surface_origin_extent.xz) / extent
    );
    float edge_distance = max(normalized_from_center.x, normalized_from_center.y);
    return height * (1.0 - smoothstep(0.88, 1.0, edge_distance));
}

struct SurfaceControlPoint {
    precise float3 world_position : POSITION;
};

struct SurfacePatchConstants {
    float edge_tessellation[3] : SV_TessFactor;
    float inside_tessellation  : SV_InsideTessFactor;
};

struct SurfaceDomainOutput {
    float4 position       : SV_POSITION;
    float3 world_position : TEXCOORD0;
    float3 world_normal   : NORMAL;
    nointerpolation float tessellation_factor : TEXCOORD1;
};

SurfaceControlPoint SurfaceVS(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID) {
    uint grid_x = max(surface_data.grid_and_options.x, 1u);
    uint grid_z = max(surface_data.grid_and_options.y, 1u);

    uint cell_x = instance_id % grid_x;
    uint cell_z = instance_id / grid_x;

    float extent = surface_data.surface_origin_extent.w;
    float2 cell_size = (2.0 * extent) / float2(grid_x, grid_z);
    float2 minimum_xz = surface_data.surface_origin_extent.xz - extent;

    uint2 corner = uint2(0u, 0u);
    switch (vertex_id) {
        case 0u: corner = uint2(0u, 0u); break;
        case 1u: corner = uint2(1u, 1u); break;
        case 2u: corner = uint2(1u, 0u); break;
        case 3u: corner = uint2(0u, 0u); break;
        case 4u: corner = uint2(0u, 1u); break;
        default: corner = uint2(1u, 1u); break;
    }

    float2 world_xz = minimum_xz + (float2(cell_x, cell_z) + float2(corner)) * cell_size;

    SurfaceControlPoint output;
    output.world_position = float3(
        world_xz.x,
        surface_data.surface_origin_extent.y,
        world_xz.y
    );
    return output;
}

float EdgeTessellationFactor(float3 endpoint_a, float3 endpoint_b) {
    float3 midpoint = 0.5 * (endpoint_a + endpoint_b);
    float edge_length = length(endpoint_b - endpoint_a);
    float maximum_height_delta =
        abs(surface_data.displacement.x) * 1.25 + abs(surface_data.displacement.y);
    float maximum_slope =
        abs(surface_data.displacement.x) * surface_data.displacement.z * 6.0 +
        abs(surface_data.displacement.y) * surface_data.displacement.w * 2.5;
    float vertical_span = min(maximum_height_delta, edge_length * maximum_slope);
    float radius = 0.5 * sqrt(edge_length * edge_length + vertical_span * vertical_span) +
                   abs(surface_data.displacement.y);

    float distance_to_edge = max(
        length(midpoint - surface_data.camera_position_tan_half_fov.xyz) - radius,
        0.1
    );
    float tan_half_fov = max(surface_data.camera_position_tan_half_fov.w, 1e-3);
    float projected_diameter_pixels =
        radius * surface_data.viewport_tessellation.y / (tan_half_fov * distance_to_edge);
    float target_pixels = max(surface_data.viewport_tessellation.z, 1.0);
    float factor = projected_diameter_pixels / target_pixels;

    float minimum_factor = max(surface_data.color_high_min_tessellation.w, 2.0);
    float maximum_factor = max(surface_data.viewport_tessellation.w, minimum_factor);
    return clamp(factor, minimum_factor, maximum_factor);
}

SurfacePatchConstants SurfacePatch(
    InputPatch<SurfaceControlPoint, 3> patch,
    uint patch_id : SV_PrimitiveID
) {
    SurfacePatchConstants output;
    output.edge_tessellation[0] = EdgeTessellationFactor(
        patch[1].world_position,
        patch[2].world_position
    );
    output.edge_tessellation[1] = EdgeTessellationFactor(
        patch[2].world_position,
        patch[0].world_position
    );
    output.edge_tessellation[2] = EdgeTessellationFactor(
        patch[0].world_position,
        patch[1].world_position
    );
    output.inside_tessellation = max(
        output.edge_tessellation[0],
        max(output.edge_tessellation[1], output.edge_tessellation[2])
    );
    return output;
}

[domain("tri")]
[partitioning("fractional_even")]
[outputtopology("triangle_ccw")]
[outputcontrolpoints(3)]
[patchconstantfunc("SurfacePatch")]
[maxtessfactor(64.0)]
SurfaceControlPoint SurfaceHS(
    InputPatch<SurfaceControlPoint, 3> patch,
    uint control_point_id : SV_OutputControlPointID,
    uint patch_id : SV_PrimitiveID
) {
    return patch[control_point_id];
}

[domain("tri")]
SurfaceDomainOutput SurfaceDS(
    SurfacePatchConstants patch_constants,
    const OutputPatch<SurfaceControlPoint, 3> patch,
    float3 barycentric : SV_DomainLocation
) {
    precise float3 base_position =
        patch[0].world_position * barycentric.x +
        patch[1].world_position * barycentric.y +
        patch[2].world_position * barycentric.z;

    float2 world_xz = base_position.xz;
    float height = EvaluateSurfaceHeight(world_xz);
    float epsilon = max(surface_data.wind_and_normal.w, 1e-3);
    float height_x = EvaluateSurfaceHeight(world_xz + float2(epsilon, 0.0));
    float height_z = EvaluateSurfaceHeight(world_xz + float2(0.0, epsilon));

    float3 tangent_x = float3(epsilon, height_x - height, 0.0);
    float3 tangent_z = float3(0.0, height_z - height, epsilon);
    float3 world_normal = normalize(cross(tangent_z, tangent_x));
    float3 world_position = float3(world_xz.x, base_position.y + height, world_xz.y);

    SurfaceDomainOutput output;
    output.position = mul(surface_data.world2clip, float4(world_position, 1.0));
    output.world_position = world_position;
    output.world_normal = world_normal;
    output.tessellation_factor =
        (patch_constants.edge_tessellation[0] +
         patch_constants.edge_tessellation[1] +
         patch_constants.edge_tessellation[2] +
         patch_constants.inside_tessellation) * 0.25;
    return output;
}

struct SurfacePixelOutput {
    float4 base_color     : SV_TARGET0;
    float4 normal         : SV_TARGET1;
    float4 metal_rough_ao : SV_TARGET2;
};

float3 TessellationHeatmap(float normalized_factor) {
    float3 cold = float3(0.05, 0.20, 0.95);
    float3 mid  = float3(0.05, 0.95, 0.35);
    float3 hot  = float3(0.95, 0.12, 0.03);
    return normalized_factor < 0.5
        ? lerp(cold, mid, normalized_factor * 2.0)
        : lerp(mid, hot, normalized_factor * 2.0 - 1.0);
}

SurfacePixelOutput SurfacePS(SurfaceDomainOutput input) {
    float3 normal = normalize(input.world_normal);
    float amplitude = max(abs(surface_data.displacement.x), 1e-3);
    float relative_height = saturate(
        (input.world_position.y - surface_data.surface_origin_extent.y) / (amplitude * 1.25)
    );
    float flatness = saturate(normal.y);
    float color_blend = surface_data.grid_and_options.z == 0u
        ? saturate(0.25 + 0.60 * relative_height + 0.15 * flatness)
        : saturate(0.15 + 0.85 * flatness);
    float3 surface_color = lerp(
        surface_data.color_low_roughness.xyz,
        surface_data.color_high_min_tessellation.xyz,
        color_blend
    );

    uint debug_mode = surface_data.grid_and_options.w;
    if (debug_mode == 1u) {
        float minimum_factor = max(surface_data.color_high_min_tessellation.w, 2.0);
        float maximum_factor = max(surface_data.viewport_tessellation.w, minimum_factor + 1e-3);
        float normalized_factor = saturate(
            (input.tessellation_factor - minimum_factor) /
            (maximum_factor - minimum_factor)
        );
        surface_color = TessellationHeatmap(normalized_factor);
    } else if (debug_mode == 2u) {
        surface_color = normal * 0.5 + 0.5;
    } else if (debug_mode == 3u) {
        surface_color = relative_height.xxx;
    }

    SurfacePixelOutput output;
    output.base_color = float4(surface_color, 0.0);
    output.normal = float4(Raster::PackNormal(normal), 1.0);
    output.metal_rough_ao = float4(0.0, surface_data.color_low_roughness.w, 1.0, 0.0);
    return output;
}
