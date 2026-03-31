/**
 * Frustum Culling Compute Shader
 * 
 * 每个线程处理一个 Draw Command (对应一个 Primitive 的所有 Instances)
 * 将不可见的 draw command 的 instance_cnt 设为 0
 */

#include "shared/scene/SharedSceneStruct.h"

struct DrawIndexedCmdData {
    uint index_cnt;
    uint instance_cnt;
    uint first_index;
    uint vertex_offset;
    uint first_instance;
};

// Culling 统计结构
struct CullStatistics {
    uint total_instances_before;  // 剔除前的总 instance 数
    uint total_instances_after;   // 剔除后的总 instance 数
    uint visible_draws;           // 可见的 draw call 数量
    uint total_draws;             // 总 draw call 数量
};

struct CullParams {
    float4 frustum_planes[6];  // World space frustum planes (nx, ny, nz, d)
    uint   draw_count;
    uint   _pad[3];
};

[[vk::push_constant]] ConstantBuffer<CullParams> cull_params;

[[vk::binding(0, 0)]] RWStructuredBuffer<DrawIndexedCmdData> draw_commands;
[[vk::binding(1, 0)]] StructuredBuffer<Moer::GPrimitive>     primitives;
[[vk::binding(2, 0)]] StructuredBuffer<Moer::GInstance>      instances;
[[vk::binding(3, 0)]] RWStructuredBuffer<CullStatistics>     statistics;  // 统计结果

/**
 * AABB-Frustum 测试
 * 使用分离轴定理（SAT）的简化版本
 * 如果 AABB 完全在某个平面的负半空间，则判定为不可见
 */
bool AABBInsideFrustum(float3 aabb_min, float3 aabb_max, float4 planes[6]) {
    float3 center = (aabb_min + aabb_max) * 0.5;
    float3 extent = (aabb_max - aabb_min) * 0.5;
    
    [unroll]
    for (int i = 0; i < 6; i++) {
        float3 normal = planes[i].xyz;
        float  distance = planes[i].w;
        
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

/**
 * 将 local AABB 变换到 world space
 */
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

[numthreads(64, 1, 1)]
void main(uint tid : SV_DispatchThreadID) {
    if (tid >= cull_params.draw_count) return;
    
    DrawIndexedCmdData cmd = draw_commands[tid];
    uint first_inst = cmd.first_instance;
    uint inst_count = cmd.instance_cnt;
    
    // 获取 primitive 的 local AABB
    Moer::GPrimitive prim = primitives[tid];
    
    uint visible_count = 0;
    
    for (uint i = 0; i < inst_count; i++) {
        Moer::GInstance inst = instances[first_inst + i];
        
        // 将 local AABB 变换到 world space
        float3 world_min, world_max;
        TransformAABB(inst.world_transform, prim.aabb_min, prim.aabb_max, world_min, world_max);
        
        // 视锥测试
        if (AABBInsideFrustum(world_min, world_max, cull_params.frustum_planes)) {
            visible_count++;
        }
    }
    
    // 写入剔除后的 instance 数量
    // 如果 visible_count == 0，GPU 会跳过这个 draw
    draw_commands[tid].instance_cnt = visible_count;
    
    // 统计
    InterlockedAdd(statistics[0].total_draws, 1);
    InterlockedAdd(statistics[0].total_instances_before, inst_count);
    InterlockedAdd(statistics[0].total_instances_after, visible_count);
    if (visible_count > 0) {
        InterlockedAdd(statistics[0].visible_draws, 1);
    }
}
