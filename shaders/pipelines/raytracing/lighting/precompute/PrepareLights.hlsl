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

float BuildTriangleLightInfo(
    float3 p0, float3 p1, float3 p2, float3 radiance, out Moer::PolymorphicLightInfo light_info
) {
    Moer::TriangleLight tri_light = (Moer::TriangleLight)0;
    tri_light.v0                  = p0;
    tri_light.edge1               = p1 - p0;
    tri_light.edge2               = p2 - p0;
    tri_light.radiance            = radiance;
    tri_light.area                = 0.5f * length(cross(tri_light.edge1, tri_light.edge2));

    light_info = tri_light.ToLightInfo();
    return tri_light.GetPower();
}

void BuildTriangleUvSampleGrad(
    float2 uv0, float2 uv1, float2 uv2, out float2 center_uv, out float2 short_grad, out float2 long_grad
) {
    float2 edge01 = uv1 - uv0;
    float2 edge02 = uv2 - uv0;
    float2 edge12 = uv2 - uv1;

    float len01_sq = dot(edge01, edge01);
    float len02_sq = dot(edge02, edge02);
    float len12_sq = dot(edge12, edge12);

    center_uv = (uv0 + uv1 + uv2) * (1.0f / 3.0f);

    if (len01_sq < len02_sq && len01_sq < len12_sq) {
        short_grad = edge01 * (2.0f / 3.0f);
        long_grad  = (edge02 + edge12) * (1.0f / 3.0f);
    } else if (len02_sq < len01_sq && len02_sq < len12_sq) {
        short_grad = edge02 * (2.0f / 3.0f);
        long_grad  = (edge01 + edge12) * (1.0f / 3.0f);
    } else {
        short_grad = edge12 * (2.0f / 3.0f);
        long_grad  = (edge01 + edge02) * (1.0f / 3.0f);
    }
}

bool IsValidBindlessHandle(uint handle) {
    return int(handle) >= 0;
}

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

[numthreads(256, 1, 1)] void main(uint dtid : SV_DispatchThreadID) {
    Moer::PrepareLightsTask task = (Moer::PrepareLightsTask)0;
    if (!FindTask(dtid, task)) {
        return;
    }

    uint tri_idx       = dtid - task.light_offset;
    bool is_prim_light = (task.primitive_id & Moer::g_task_prim_light_bit) != 0;

    Moer::PolymorphicLightInfo light_info = (Moer::PolymorphicLightInfo)0;
    float                      light_power = 0.0f;

    if (is_prim_light) {
        // ============================================================================
        // 处理场景光源（Directional Light, Point Light 等）
        // ============================================================================
        uint prim_light_idx = task.primitive_id & ~Moer::g_task_prim_light_bit;
        light_info          = prim_lights[prim_light_idx];
        light_power         = Moer::PolymorphicLight::GetPower(light_info);
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
        // GPrimitive 已经有 index_start_idx，CPU 侧从 CPrimitive.index.start_idx 填充。
        // PrepareLightsTask 也保存同一个起点，单位是 uint 元素，不是字节偏移。

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
        uint  index_start_idx = task.index_start_idx;
        uint3 idx             = index_buf.Load<uint3>(tri_idx, index_start_idx * sizeof(uint));

        // 6. 从 MegaBuffers 读取顶点位置
        ArrayBuffer position_buf = ArrayBuffer(param.position_buf_hdl);
        float3      p0;
        float3      p1;
        float3      p2;
        if (primitive.attribute_mask & Moer::GPrimitiveEAttributeMask::Position) {
            uint position_start_idx = primitive.position_start_idx;
            p0                      = position_buf.Load<float3>(position_start_idx + idx.x);
            p1                      = position_buf.Load<float3>(position_start_idx + idx.y);
            p2                      = position_buf.Load<float3>(position_start_idx + idx.z);
        } else {
            // 如果没有 position，使用默认值（这种情况不应该发生）
            p0 = float3(0, 0, 0);
            p1 = float3(0, 0, 0);
            p2 = float3(0, 0, 0);
        }

        // 7. 应用 world transform
        p0 = mul(model2world, float4(p0, 1.0f)).xyz;
        p1 = mul(model2world, float4(p1, 1.0f)).xyz;
        p2 = mul(model2world, float4(p2, 1.0f)).xyz;

        // 8. 获取自发光颜色
        float3 emissive = material.emissive_factor;

        if (IsValidBindlessHandle(material.emissive_map_hdl)) {
            TextureHandle emissive_tex = (TextureHandle)material.emissive_map_hdl;
            float2        uv0;
            float2        uv1;
            float2        uv2;
            // 从 MegaBuffers 读取 UV 坐标
            if (primitive.attribute_mask & Moer::GPrimitiveEAttributeMask::Texcoord0) {
                ArrayBuffer texcoord0_buf = ArrayBuffer(param.texcoord0_buf_hdl);
                uint        texcoord0_start_idx = primitive.texcoord0_start_idx;
                uv0                             = texcoord0_buf.Load<float2>(texcoord0_start_idx + idx.x);
                uv1                             = texcoord0_buf.Load<float2>(texcoord0_start_idx + idx.y);
                uv2                             = texcoord0_buf.Load<float2>(texcoord0_start_idx + idx.z);
            } else {
                // 如果没有 UV，使用默认值（这种情况不应该发生，但为了安全起见）
                uv0 = float2(0, 0);
                uv1 = float2(0, 0);
                uv2 = float2(0, 0);
            }

            float2 center_uv;
            float2 short_grad;
            float2 long_grad;
            BuildTriangleUvSampleGrad(uv0, uv1, uv2, center_uv, short_grad, long_grad);
            float3 emissive_mask = emissive_tex.SampleGrad<float3>(center_uv, short_grad, long_grad);
            emissive *= emissive_mask;
        }

        emissive.rgb = max(emissive.rgb, 0.0f);
        light_power  = BuildTriangleLightInfo(p0, p1, p2, emissive, light_info);
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

    uint2 pdf_position            = Math::LinearIndexToZCurve(light_buf_idx);
    local_light_pdf[pdf_position] = light_power;
}
