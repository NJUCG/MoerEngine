#include <core/common/Bindless.hlsl>
#include <core/common/Common.hlsl>
#include <shared/Geometry.h>
#include <shared/lighting/ShaderParameters.h>
#include <shared/utils/MoerMath.hlsli>

BINDLESS_BINDINGS(3, 2, 4, 5)
#include "shared/scene/SharedSceneStruct.h"
#include <materials/Material.hlsli>
#include <pipelines/raytracing/lighting/common/PolymorphicLight.hlsli>
#include <shared/utils/Packing.h>

[[vk::push_constant]] ConstantBuffer<Moer::PrepareLightsParams> param;

[[vk::binding(0, 0)]] RWStructuredBuffer<Moer::PolymorphicLightInfo> light_data : register(u0);
[[vk::binding(1, 0)]] RWBuffer<uint>                                 light_index_mapping : register(u1);

[[vk::binding(2, 0)]] RWTexture2D<float> local_light_pdf;

[[vk::binding(3, 0)]] StructuredBuffer<Moer::PolymorphicLightInfo> prim_lights;
[[vk::binding(4, 0)]] StructuredBuffer<Moer::PrepareLightsTask>    tasks;

bool FindTask(uint dtid, out Moer::PrepareLightsTask task) {
    // binary search in task buffer
    int left  = 0;
    int right = int(param.num_tasks) - 1;

    while (left <= right) {
        int mid = (left + right) / 2;
        task    = tasks[mid];
        int tri = int(dtid) - int(task.light_offset);
        if (tri < 0) {
            right = mid - 1;
        } else if (tri >= int(task.num_triangles)) {
            left = mid + 1;
        } else {
            return true;
        }
    }
    return false;
}

[numthreads(256, 1, 1)] void main(uint dtid : SV_DispatchThreadID, uint gtid : SV_GroupThreadID) {
    Moer::PrepareLightsTask task = (Moer::PrepareLightsTask)0;
    if (!FindTask(dtid, task)) {
        return;
    }

    uint tri_idx       = dtid - task.light_offset;
    bool is_prim_light = (task.primitive_id & Moer::g_task_prim_light_bit) != 0;

    Moer::PolymorphicLightInfo light_info = (Moer::PolymorphicLightInfo)0;

    if (is_prim_light) {
        // ============================================================================
        // 处理场景光源（Directional Light, Point Light 等）
        // ============================================================================
        uint prim_light_idx = task.primitive_id & ~Moer::g_task_prim_light_bit;
        light_info          = prim_lights[prim_light_idx];
        uint type           = Moer::GetLightType(light_info);
    } else {
        // ============================================================================
        // 新架构：处理自发光三角形
        // ============================================================================
        // primitive_id 现在直接存储 primitive_id（不再需要解析 instance_id 和 geo_idx）
        // 需要：
        // 1. 从 primitive_id 加载 GPrimitive
        // 2. 从 GPrimitive 获取 material_id，加载 GMaterial
        // 3. 从 MegaBuffers 读取顶点数据（position, index）
        // 4. 找到对应的 GInstance 获取 world_transform（使用第一个匹配的 Instance）
        // ============================================================================
        uint primitive_id = task.primitive_id; // 直接使用，不再需要解析

        // 1. 加载 GPrimitive
        ArrayBuffer      primitive_buf = ArrayBuffer(param.primitive_buf_hdl);
        Moer::GPrimitive primitive     = primitive_buf.Load<Moer::GPrimitive>(primitive_id);

        // 2. 加载 GMaterial
        ArrayBuffer     material_buf = ArrayBuffer(param.material_buf_hdl);
        Moer::GMaterial material     = material_buf.Load<Moer::GMaterial>(primitive.material_idx);

        // 3. 从 MegaBuffers 读取索引数据
        ArrayBuffer index_buf = ArrayBuffer(param.index_buf_hdl);
        // 注意：GPrimitive 中没有直接的 index_offset，需要从 CPrimitive 的 index.offset 获取
        // 但在新架构中，index 是连续的，所以我们需要知道每个 Primitive 的起始索引
        // 实际上，在 CpuScene 中，每个 Primitive 的索引是连续的，所以我们需要：
        // - 计算该 Primitive 的起始索引：需要知道前面所有 Primitive 的 index_count 之和
        // 但这样会很复杂。让我们先假设 index_offset 存储在 GPrimitive 中（如果没有，需要添加）
        // 实际上，在新架构中，index 是全局的，所以我们需要知道每个 Primitive 的起始位置
        // 暂时使用一个简化的方法：假设 index 从 0 开始，每个 Primitive 的索引是连续的
        // TODO: 需要确认 GPrimitive 中是否有 index_offset 字段，或者需要添加

        // 4. 获取对应的 GInstance（使用第一个 Instance）
        // 注意：一个 Primitive 可能有多个 Instance，这里我们使用第一个
        // C++ 代码已经为我们计算了第一个 Instance 的索引并存储在 task.first_instance_idx 中
        ArrayBuffer instance_buf = ArrayBuffer(param.instance_buf_hdl);
        float4x4    model2world  = float4x4(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1); // 默认单位矩阵
        if (task.first_instance_idx != 0xFFFFFFFF) {                                         // UINT_MAX
            Moer::GInstance instance = instance_buf.Load<Moer::GInstance>(task.first_instance_idx);
            model2world              = instance.world_transform;
        }

        // 5. 读取三角形索引
        // 使用 task.index_start_idx 获取该 Primitive 的起始索引位置
        uint  index_offset = task.index_start_idx;
        uint3 idx;
        idx.x = index_buf.Load<uint>(index_offset + tri_idx * 3);
        idx.y = index_buf.Load<uint>(index_offset + tri_idx * 3 + 1);
        idx.z = index_buf.Load<uint>(index_offset + tri_idx * 3 + 2);

        // 6. 从 MegaBuffers 读取顶点位置
        ArrayBuffer position_buf = ArrayBuffer(param.position_buf_hdl);
        float3      positions[3];
        if (primitive.attribute_mask & Moer::GPrimitiveEAttributeMask::Position) {
            positions[0] = position_buf.Load<float3>(primitive.position_start_idx + idx.x);
            positions[1] = position_buf.Load<float3>(primitive.position_start_idx + idx.y);
            positions[2] = position_buf.Load<float3>(primitive.position_start_idx + idx.z);
        } else {
            // 如果没有 position，使用默认值（这种情况不应该发生）
            positions[0] = float3(0, 0, 0);
            positions[1] = float3(0, 0, 0);
            positions[2] = float3(0, 0, 0);
        }

        // 7. 应用 world transform
        positions[0] = mul(model2world, float4(positions[0], 1.0f)).xyz;
        positions[1] = mul(model2world, float4(positions[1], 1.0f)).xyz;
        positions[2] = mul(model2world, float4(positions[2], 1.0f)).xyz;

        // 8. 获取自发光颜色
        float3 emissive = material.emissive_factor;

        // TODO: handle emissive texture
        // printf("mat.emissive_map %d\n", mat.emissive_map);
        if (material.emissive_map_hdl > 0) {
            TextureHandle emissive_tex = (TextureHandle)material.emissive_map_hdl;
            float2        uvs[3];
            // 从 MegaBuffers 读取 UV 坐标
            if (primitive.attribute_mask & Moer::GPrimitiveEAttributeMask::Texcoord0) {
                ArrayBuffer texcoord0_buf = ArrayBuffer(param.texcoord0_buf_hdl);
                uvs[0]                    = texcoord0_buf.Load<float2>(primitive.texcoord0_start_idx + idx.x);
                uvs[1]                    = texcoord0_buf.Load<float2>(primitive.texcoord0_start_idx + idx.y);
                uvs[2]                    = texcoord0_buf.Load<float2>(primitive.texcoord0_start_idx + idx.z);
            } else {
                // 如果没有 UV，使用默认值（这种情况不应该发生，但为了安全起见）
                uvs[0] = float2(0, 0);
                uvs[1] = float2(0, 0);
                uvs[2] = float2(0, 0);
            }

            float2 edges[3];
            edges[0] = uvs[1] - uvs[0];
            edges[1] = uvs[2] - uvs[0];
            edges[2] = uvs[2] - uvs[1];

            float3 edge_lengths = float3(length(edges[0]), length(edges[1]), length(edges[2]));
            float2 short_edge;
            float2 long_edge1;
            float2 long_edge2;

            if (edge_lengths.x < edge_lengths.y && edge_lengths.x < edge_lengths.z) {
                short_edge = edges[0];
                long_edge1 = edges[1];
                long_edge2 = edges[2];
            } else if (edge_lengths.y < edge_lengths.x && edge_lengths.y < edge_lengths.z) {
                short_edge = edges[1];
                long_edge1 = edges[0];
                long_edge2 = edges[2];
            } else {
                short_edge = edges[2];
                long_edge1 = edges[0];
                long_edge2 = edges[1];
            }

            float2 short_grad = short_edge * 2.f / 3.f;
            float2 long_grad  = (long_edge1 + long_edge2) / 3.f;

            float2 center_uv     = (uvs[0] + uvs[1] + uvs[2]) / 3.f;
            float3 emissive_mask = emissive_tex.SampleGrad<float3>(center_uv, short_grad, long_grad);
            emissive *= emissive_mask;

            Moer::TriangleIndirectLight tri_light_indirect = (Moer::TriangleIndirectLight)0;
            tri_light_indirect.v0                          = positions[0];
            tri_light_indirect.edge1                       = positions[1] - positions[0];
            tri_light_indirect.edge2                       = positions[2] - positions[0];
            tri_light_indirect.avg_radiance                = emissive;
            tri_light_indirect.uv0                         = uvs[0];
            tri_light_indirect.edge_uv1                    = uvs[1] - uvs[0];
            tri_light_indirect.edge_uv2                    = uvs[2] - uvs[0];
            tri_light_indirect.tex_handle                  = material.emissive_map_hdl;

            // printf("src avg_radiance %f %f %f uv0 %f %f edg_uv1 %f %f edg_uv2 %f %f tex_handle %d\n", tri_light_indirect.avg_radiance.x, tri_light_indirect.avg_radiance.y, tri_light_indirect.avg_radiance.z, tri_light_indirect.uv0.x, tri_light_indirect.uv0.y, tri_light_indirect.edge_uv1.x, tri_light_indirect.edge_uv1.y, tri_light_indirect.edge_uv2.x, tri_light_indirect.edge_uv2.y, tri_light_indirect.tex_handle);

            // light_info = tri_light_indirect.ToLightInfo();

            // Moer::TriangleIndirectLight tri_light_indirect2 = (Moer::TriangleIndirectLight)0;
            // tri_light_indirect2 = Moer::TriangleIndirectLight::Create(light_info);
            // printf("dst avg_radiance %f %f %f uv0 %f %f edg_uv1 %f %f edg_uv2 %f %f tex_handle %d\n", tri_light_indirect2.avg_radiance.x, tri_light_indirect2.avg_radiance.y, tri_light_indirect2.avg_radiance.z, tri_light_indirect2.uv0.x, tri_light_indirect2.uv0.y, tri_light_indirect2.edge_uv1.x, tri_light_indirect2.edge_uv1.y, tri_light_indirect2.edge_uv2.x, tri_light_indirect2.edge_uv2.y, tri_light_indirect2.tex_handle);

            // return;
        }

        emissive.rgb                  = max(emissive.rgb, 0.0f);
        Moer::TriangleLight tri_light = (Moer::TriangleLight)0;
        tri_light.v0                  = positions[0];
        tri_light.edge1               = positions[1] - positions[0];
        tri_light.edge2               = positions[2] - positions[0];
        tri_light.radiance            = emissive;

        light_info = tri_light.ToLightInfo();
    }

    uint light_buf_idx                                 = task.light_offset + tri_idx;
    light_data[param.cur_light_offset + light_buf_idx] = light_info;

    if (task.prev_light_offset >= 0) {
        uint prev_light_buf_idx = task.prev_light_offset + tri_idx;
        light_index_mapping[prev_light_buf_idx + param.prev_light_offset] =
            light_buf_idx + param.cur_light_offset + 1;

        light_index_mapping[light_buf_idx + param.cur_light_offset] =
            prev_light_buf_idx + param.prev_light_offset + 1;
    }

    float emissive_flux           = Moer::PolymorphicLight::GetPower(light_info);
    uint2 pdf_position            = Math::LinearIndexToZCurve(light_buf_idx);
    local_light_pdf[pdf_position] = emissive_flux;
}