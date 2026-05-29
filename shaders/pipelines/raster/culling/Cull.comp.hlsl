/**
 * Culling Compute Shader
 * 
 * 每个线程处理一个 Primitive 对应的源 Draw Command，
 * 为当前 pass 生成紧凑的可见实例索引和新的间接绘制命令。
 */

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

// Culls each source draw and emits compact visible-instance and indirect-draw buffers.
[numthreads(64, 1, 1)]
void main(uint tid : SV_DispatchThreadID) {
    if (tid >= cull_params.draw_count) return;
    
    Moer::DrawIndexedCmdData src_cmd = source_draw_commands[tid];
    uint first_inst = src_cmd.first_instance;
    uint inst_count = src_cmd.instance_cnt;
    
    // 获取 primitive 的 local AABB
    Moer::GPrimitive prim = primitives[tid];
    
    uint visible_count = 0;
    
    for (uint i = 0; i < inst_count; i++) {
        Moer::GInstance inst = instances[first_inst + i];
        
        // 将 local AABB 变换到 world space
        float3 world_min, world_max;
        TransformAABB(inst.world_transform, prim.aabb_min, prim.aabb_max, world_min, world_max);
        
        // 视锥测试
        if (AABBInsideFrustum(world_min, world_max)) {
            visible_count++;
        }
    }
    
    InterlockedAdd(counters[0].total_draws, 1);
    InterlockedAdd(counters[0].total_instances_before, inst_count);
    InterlockedAdd(counters[0].total_instances_after, visible_count);

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

        float3 world_min, world_max;
        TransformAABB(inst.world_transform, prim.aabb_min, prim.aabb_max, world_min, world_max);

        if (AABBInsideFrustum(world_min, world_max)) {
            visible_instance_ids[visible_instance_offset + write_offset] = first_inst + i;
            write_offset++;
        }
    }

    Moer::DrawIndexedCmdData dst_cmd = src_cmd;
    dst_cmd.first_instance = visible_instance_offset;
    dst_cmd.instance_cnt   = visible_count;
    draw_commands[draw_index] = dst_cmd;
}