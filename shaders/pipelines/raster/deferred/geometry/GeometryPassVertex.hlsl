#ifndef SHADOW_DEPTH_PASS
#define SHADOW_DEPTH_PASS 0
#endif

#include "core/common/Bindless.hlsl"
#include "core/common/Common.hlsl"
#include "shared/ShaderParameters.h"
BINDLESS_BINDINGS(3, 2, 4, 5)
#include "shared/Geometry.h"
#include "shared/raster/ShaderParameters.h"
#include "shared/scene/SharedSceneStruct.h"
#include "shared/utils/Packing.h"

#include "pipelines/raster/deferred/geometry/GeometryPassCommon.hlsli"

[[vk::push_constant]] ConstantBuffer<Moer::GeometryPassBindlessParam> param;

#if SHADOW_DEPTH_PASS

VsOutput main(
    uint vertex_id : SV_VertexID,      // 顶点 ID（自动由 GPU 提供）
    uint instance_id : SV_InstanceID   // 实例 ID（自动由 GPU 提供）
) {
    // 1. GInstance
    ArrayBuffer instance_buf = ArrayBuffer(param.instance_buf_hdl);
    Moer::GInstance instance = instance_buf.Load<Moer::GInstance>(instance_id);
    uint primitive_id = instance.primitive_id; // primitive_id == draw_id
    float4x4 model2world = instance.world_transform;

    // 2. GPrimitive
    ArrayBuffer primitive_buf = ArrayBuffer(param.primitive_buf_hdl);
    Moer::GPrimitive primitive = primitive_buf.Load<Moer::GPrimitive>(primitive_id);

    // 3. Read from MegaBuffers

    // position
    float3 vertex_pos = float3(0, 0, 0);
    if (primitive.attribute_mask & Moer::GPrimitiveEAttributeMask::Position) {
        ArrayBuffer position_buf = ArrayBuffer(param.position_buf_hdl);
        vertex_pos = position_buf.Load<float3>(primitive.position_start_idx + vertex_id);
    }
    float3 world_pos = mul(model2world, float4(vertex_pos, 1.0));
    float4 clip_pos = mul(param.world2clip, float4(world_pos, 1.0));

    // texcoord0
    float2 texcoord0 = float2(0, 0);
    if (primitive.attribute_mask & Moer::GPrimitiveEAttributeMask::Texcoord0) {
        ArrayBuffer texcoord0_buf = ArrayBuffer(param.texcoord0_buf_hdl);
        texcoord0 = texcoord0_buf.Load<float2>(primitive.texcoord0_start_idx + vertex_id);
    }

    // 4. Transform

    VsOutput output;

    output.position = clip_pos;
    output.texcoord0 = texcoord0;
    output.material_id = primitive.material_idx;

    return output;
}

#else

VsOutput main(
    uint vertex_id : SV_VertexID,      // 顶点 ID（自动由 GPU 提供）
    uint instance_id : SV_InstanceID   // 实例 ID（自动由 GPU 提供）
) {
    // 1. GInstance
    ArrayBuffer instance_buf = ArrayBuffer(param.instance_buf_hdl);
    Moer::GInstance instance = instance_buf.Load<Moer::GInstance>(instance_id);
    uint primitive_id = instance.primitive_id; // primitive_id == draw_id
    float4x4 model2world = instance.world_transform;
    float3x3 model2world_3x3 = (float3x3)model2world; // FIXME: use NormalMatrix to transform normal

    // 2. GPrimitive
    ArrayBuffer primitive_buf = ArrayBuffer(param.primitive_buf_hdl);
    Moer::GPrimitive primitive = primitive_buf.Load<Moer::GPrimitive>(primitive_id);

    // 3. Read from MegaBuffers

    // position
    float3 vertex_pos = float3(0, 0, 0);
    if (primitive.attribute_mask & Moer::GPrimitiveEAttributeMask::Position) {
        ArrayBuffer position_buf = ArrayBuffer(param.position_buf_hdl);
        vertex_pos = position_buf.Load<float3>(primitive.position_start_idx + vertex_id);
    }
    float3 world_pos = mul(model2world, float4(vertex_pos, 1.0));
    float4 clip_pos = mul(param.world2clip, float4(world_pos, 1.0));

    // normal
    float3 normal = float3(0, 0, 1);
    if (primitive.attribute_mask & Moer::GPrimitiveEAttributeMask::PackedNormal) {
        ArrayBuffer normal_buf = ArrayBuffer(param.normal_buf_hdl);
        uint packed_normal = normal_buf.Load<uint>(primitive.packed_normal_start_idx + vertex_id);
        normal = Moer::Unpack_Normal(packed_normal);
    }

    // tangent
    float3 tangent = float3(0, 0, 0);
    if (primitive.attribute_mask & Moer::GPrimitiveEAttributeMask::PackedTangent) {
        ArrayBuffer tangent_buf = ArrayBuffer(param.tangent_buf_hdl);
        uint packed_tangent = tangent_buf.Load<uint>(primitive.packed_tangent_start_idx + vertex_id);
        tangent = Moer::Unpack_Normal(packed_tangent);
    } else {
        tangent = cross(normal, float3(0, 0, 1));
        if (length(tangent) < 1e-2) {
            tangent = cross(normal, float3(0, 1, 0));
        }
        tangent = normalize(tangent);
    }

    // texcoord0
    float2 texcoord0 = float2(0, 0);
    if (primitive.attribute_mask & Moer::GPrimitiveEAttributeMask::Texcoord0) {
        ArrayBuffer texcoord0_buf = ArrayBuffer(param.texcoord0_buf_hdl);
        texcoord0 = texcoord0_buf.Load<float2>(primitive.texcoord0_start_idx + vertex_id);
    }

    // 4. Transform

    VsOutput output;

    output.position = clip_pos;
    output.world_position = world_pos;
    output.normal = normalize(mul(model2world_3x3, normal));
    output.tangent = normalize(mul(model2world_3x3, tangent));
    output.texcoord0 = texcoord0;
    output.material_id = primitive.material_idx;

    return output;
}

#endif