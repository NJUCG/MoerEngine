/**
 * Culling Compute Shader
 * 
 * 每个线程处理一个 Primitive 对应的源 Draw Command，
 * 为当前 pass 生成紧凑的可见实例索引和新的间接绘制命令。
 */

#ifndef ENABLE_HIZ_OCCLUSION
#define ENABLE_HIZ_OCCLUSION 0
#endif

#if ENABLE_HIZ_OCCLUSION
#include "core/common/Bindless.hlsl"
BINDLESS_BINDINGS(3, 2, 4, 5)
#endif

#include "shared/raster/culling/ShaderParameters.h"
#include "shared/rhi/CommandDrawData.h"
#include "shared/scene/SharedSceneStruct.h"

[[vk::push_constant]] ConstantBuffer<Moer::CullParams> cull_params;

[[vk::binding(0, 0)]] StructuredBuffer<Moer::DrawIndexedCmdData> source_draw_commands;
[[vk::binding(1, 0)]] StructuredBuffer<Moer::GPrimitive> primitives;
[[vk::binding(2, 0)]] StructuredBuffer<Moer::GInstance> instances;
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> visible_instance_ids;
[[vk::binding(4, 0)]] RWStructuredBuffer<Moer::DrawIndexedCmdData> draw_commands;
[[vk::binding(5, 0)]] RWStructuredBuffer<Moer::GpuCullingCounterData> counters;
[[vk::binding(6, 0)]] ConstantBuffer<Moer::CullData> cull_data;

// Tests whether a world-space AABB intersects the current frustum conservatively.
bool AABBInsideFrustum(float3 aabb_min, float3 aabb_max) {
    float3 center = (aabb_min + aabb_max) * 0.5;
    float3 extent = (aabb_max - aabb_min) * 0.5;
    
    [unroll]
    for (int i = 0; i < 6; i++) {
        float3 normal = cull_data.frustum_planes[i].xyz;
        float  distance = cull_data.frustum_planes[i].w;
        
        // 计算 AABB 在平面法线方向上的投影半径
        float radius = dot(extent, abs(normal));
        float center_dist = dot(center, normal) + distance;
        
        // 如果 center 到平面的距离小于 -radius，则 AABB 完全在平面外侧
        if (center_dist < -radius) {
            return false;
        }
    }
    return true;
}

// Transforms a local-space AABB into a conservative world-space AABB.
void TransformAABB(float4x4 transform, float3 local_min, float3 local_max, 
                   out float3 out_min, out float3 out_max) {
    float3 corners[8];
    corners[0] = local_min;
    corners[1] = float3(local_max.x, local_min.y, local_min.z);
    corners[2] = float3(local_min.x, local_max.y, local_min.z);
    corners[3] = float3(local_max.x, local_max.y, local_min.z);
    corners[4] = float3(local_min.x, local_min.y, local_max.z);
    corners[5] = float3(local_max.x, local_min.y, local_max.z);
    corners[6] = float3(local_min.x, local_max.y, local_max.z);
    corners[7] = local_max;
    
    out_min = float3(1e30, 1e30, 1e30);
    out_max = float3(-1e30, -1e30, -1e30);
    
    [unroll]
    for (int i = 0; i < 8; i++) {
        float3 world_pos = mul(transform, float4(corners[i], 1.0)).xyz;
        out_min = min(out_min, world_pos);
        out_max = max(out_max, world_pos);
    }
}

#if ENABLE_HIZ_OCCLUSION

uint GetHiZMipForScreenBounds(float2 uv_min, float2 uv_max) {
    float2 hiz_size    = float2(cull_data.hiz_info.xy);
    float2 pixel_size  = max((uv_max - uv_min) * hiz_size, float2(1.0, 1.0));
    float  max_pixels  = max(pixel_size.x, pixel_size.y);
    uint   target_mip  = (uint)ceil(log2(max_pixels));
    uint   max_mip_idx = cull_params.hiz_mip_count - 1;
    return min(target_mip, max_mip_idx);
}

float SamplePreviousHiZ(uint mip, float2 uv) {
    return TextureHandle(cull_params.hiz_mip_handles[mip]).SampleLevel<float>(saturate(uv), 0.0);
}

// Tests occlusion against the previous frame Hi-Z pyramid. Reverse-Z stores the farthest depth per mip tile.
bool AABBOccludedByPreviousHiZ(float3 aabb_min, float3 aabb_max) {
    if ((cull_params.flags & Moer::CULL_FLAG_ENABLE_HIZ_OCCLUSION) == 0 || cull_params.hiz_mip_count == 0) {
        return false;
    }

    float3 corners[8];
    corners[0] = aabb_min;
    corners[1] = float3(aabb_max.x, aabb_min.y, aabb_min.z);
    corners[2] = float3(aabb_min.x, aabb_max.y, aabb_min.z);
    corners[3] = float3(aabb_max.x, aabb_max.y, aabb_min.z);
    corners[4] = float3(aabb_min.x, aabb_min.y, aabb_max.z);
    corners[5] = float3(aabb_max.x, aabb_min.y, aabb_max.z);
    corners[6] = float3(aabb_min.x, aabb_max.y, aabb_max.z);
    corners[7] = aabb_max;

    float2 ndc_min = float2(1e30, 1e30);
    float2 ndc_max = float2(-1e30, -1e30);
    float  nearest_depth = 0.0;

    [unroll]
    for (int i = 0; i < 8; ++i) {
        float4 clip = mul(cull_data.previous_view_proj, float4(corners[i], 1.0));
        if (clip.w <= 1e-6) {
            return false;
        }

        float3 ndc = clip.xyz / clip.w;
        if (ndc.z < 0.0 || ndc.z > 1.0) {
            return false;
        }

        ndc_min = min(ndc_min, ndc.xy);
        ndc_max = max(ndc_max, ndc.xy);
        nearest_depth = max(nearest_depth, ndc.z);
    }

    if (ndc_min.x < -1.0 || ndc_min.y < -1.0 || ndc_max.x > 1.0 || ndc_max.y > 1.0) {
        return false;
    }

    float2 uv_min = float2(ndc_min.x * 0.5 + 0.5, 0.5 - ndc_max.y * 0.5);
    float2 uv_max = float2(ndc_max.x * 0.5 + 0.5, 0.5 - ndc_min.y * 0.5);
    uint   mip    = GetHiZMipForScreenBounds(uv_min, uv_max);

    float hiz_depth = min(
        min(SamplePreviousHiZ(mip, uv_min), SamplePreviousHiZ(mip, float2(uv_max.x, uv_min.y))),
        min(SamplePreviousHiZ(mip, float2(uv_min.x, uv_max.y)), SamplePreviousHiZ(mip, uv_max))
    );

    return hiz_depth > nearest_depth + 1e-4;
}

#endif

bool IsInstanceVisible(Moer::GInstance inst, Moer::GPrimitive prim, out bool frustum_culled, out bool occlusion_culled) {
    float3 world_min, world_max;
    TransformAABB(inst.world_transform, prim.aabb_min, prim.aabb_max, world_min, world_max);

    frustum_culled   = false;
    occlusion_culled = false;

    if (!AABBInsideFrustum(world_min, world_max)) {
        frustum_culled = true;
        return false;
    }

#if ENABLE_HIZ_OCCLUSION
    if (AABBOccludedByPreviousHiZ(world_min, world_max)) {
        occlusion_culled = true;
        return false;
    }
#endif

    return true;
}

// Culls each source draw and emits compact visible-instance and indirect-draw buffers.
[numthreads(64, 1, 1)]
void main(uint tid : SV_DispatchThreadID) {
    if (tid >= cull_params.draw_count) return;
    
    Moer::DrawIndexedCmdData src_cmd = source_draw_commands[tid];
    uint first_inst = src_cmd.first_instance;
    uint inst_count = src_cmd.instance_cnt;
    
    // 获取 primitive 的 local AABB
    Moer::GPrimitive prim = primitives[tid];
    
    uint visible_count    = 0;
    uint frustum_culled   = 0;
    uint occlusion_culled = 0;
    
    for (uint i = 0; i < inst_count; i++) {
        Moer::GInstance inst = instances[first_inst + i];

        bool is_frustum_culled;
        bool is_occlusion_culled;
        if (IsInstanceVisible(inst, prim, is_frustum_culled, is_occlusion_culled)) {
            visible_count++;
        } else if (is_frustum_culled) {
            frustum_culled++;
        } else if (is_occlusion_culled) {
            occlusion_culled++;
        }
    }
    
    InterlockedAdd(counters[0].total_draws, 1);
    InterlockedAdd(counters[0].total_instances_before, inst_count);
    InterlockedAdd(counters[0].total_instances_after, visible_count);
    InterlockedAdd(counters[0].frustum_culled_instances, frustum_culled);
    InterlockedAdd(counters[0].occlusion_culled_instances, occlusion_culled);

    if (visible_count == 0) {
        return;
    }

    uint visible_instance_offset;
    InterlockedAdd(counters[0].visible_instance_count, visible_count, visible_instance_offset);

    uint draw_index;
    InterlockedAdd(counters[0].draw_count, 1, draw_index);
    InterlockedAdd(counters[0].visible_draws, 1);

    uint write_offset = 0;
    for (uint i = 0; i < inst_count; i++) {
        Moer::GInstance inst = instances[first_inst + i];

        bool is_frustum_culled;
        bool is_occlusion_culled;
        if (IsInstanceVisible(inst, prim, is_frustum_culled, is_occlusion_culled)) {
            visible_instance_ids[visible_instance_offset + write_offset] = first_inst + i;
            write_offset++;
        }
    }

    Moer::DrawIndexedCmdData dst_cmd = src_cmd;
    dst_cmd.first_instance = visible_instance_offset;
    dst_cmd.instance_cnt   = visible_count;
    draw_commands[draw_index] = dst_cmd;
}