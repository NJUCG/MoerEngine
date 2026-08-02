#ifndef SHADOW_DEPTH_PASS
#define SHADOW_DEPTH_PASS 0
#endif

#ifndef POINT_SHADOW_MULTIVIEW
#define POINT_SHADOW_MULTIVIEW 0
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

#if POINT_SHADOW_MULTIVIEW
[[vk::binding(0, 0)]] ConstantBuffer<Moer::PointShadowViewMatrices> point_shadow_view_matrices;
#endif

struct VsContext {
    // input
    uint vertex_id;   // 顶点 ID（自动由 GPU 提供）
    uint instance_id; // 实例 ID（自动由 GPU 提供）

    // derived
    uint     primitive_id;
    float4x4 model2world;

    Moer::GPrimitive primitive;

    // derived 2
    float4 out_clip_pos;

#if !SHADOW_DEPTH_PASS
    float3x3 model2world_3x3;

    float3 out_normal;
    float3 out_tangent;
#endif

    float2 out_texcoord0;
    uint   out_material_id;

    // funcs

    void Load(uint _vertex_id, uint _instance_id, uint _view_id) {
        vertex_id   = _vertex_id;
        instance_id = _instance_id;

        // 1. GInstance
        uint scene_instance_id = instance_id;
        if (param.use_visible_instance_id_remap != 0) {
            ArrayBuffer visible_instance_id_buf = ArrayBuffer(param.visible_instance_id_buf_hdl);
            scene_instance_id                   = visible_instance_id_buf.Load<uint>(instance_id);
        }

        ArrayBuffer     instance_buf = ArrayBuffer(param.instance_buf_hdl);
        Moer::GInstance instance     = instance_buf.Load<Moer::GInstance>(scene_instance_id);
        primitive_id                 = instance.primitive_id; // primitive_id == draw_id
        model2world                  = instance.world_transform;

#if !SHADOW_DEPTH_PASS
        model2world_3x3 = (float3x3)model2world; // FIXME: use NormalMatrix to transform normal
#endif

        // 2. GPrimitive
        ArrayBuffer primitive_buf = ArrayBuffer(param.primitive_buf_hdl);
        primitive                 = primitive_buf.Load<Moer::GPrimitive>(primitive_id);

        // 3. Read from MegaBuffers

        // position
        float3 vertex_pos = float3(0, 0, 0);
        if (primitive.attribute_mask & Moer::GPrimitiveEAttributeMask::Position) {
            ArrayBuffer position_buf = ArrayBuffer(param.position_buf_hdl);
            vertex_pos               = position_buf.Load<float3>(primitive.position_start_idx + vertex_id);
        }
        float3 world_pos = mul(model2world, float4(vertex_pos, 1.0)).xyz;
        float4x4 world2clip = param.world2clip;
#if POINT_SHADOW_MULTIVIEW
        world2clip = point_shadow_view_matrices.world2clip[min(_view_id, 5u)];
#endif
        out_clip_pos = mul(world2clip, float4(world_pos, 1.0));

#if !SHADOW_DEPTH_PASS

        // normal
        out_normal = float3(0, 0, 1);
        if (primitive.attribute_mask & Moer::GPrimitiveEAttributeMask::PackedNormal) {
            ArrayBuffer packed_normal_buf = ArrayBuffer(param.packed_normal_buf_hdl);
            uint packed_normal = packed_normal_buf.Load<uint>(primitive.packed_normal_start_idx + vertex_id);
            out_normal         = Moer::Unpack_Normal(packed_normal);
        }
        out_normal = normalize(mul(model2world_3x3, out_normal));

        // tangent
        out_tangent = float3(0, 0, 0);
        if (primitive.attribute_mask & Moer::GPrimitiveEAttributeMask::PackedTangent) {
            ArrayBuffer packed_tangent_buf = ArrayBuffer(param.packed_tangent_buf_hdl);
            uint        packed_tangent =
                packed_tangent_buf.Load<uint>(primitive.packed_tangent_start_idx + vertex_id);
            out_tangent = Moer::Unpack_Normal(packed_tangent);
        } else {
            out_tangent = cross(out_normal, float3(0, 0, 1));
            if (length(out_tangent) < 1e-2) {
                out_tangent = cross(out_normal, float3(0, 1, 0));
            }
            out_tangent = normalize(out_tangent);
        }
        out_tangent = normalize(mul(model2world_3x3, out_tangent));

#endif

        // texcoord0
        out_texcoord0 = float2(0, 0);
        if (primitive.attribute_mask & Moer::GPrimitiveEAttributeMask::Texcoord0) {
            ArrayBuffer texcoord0_buf = ArrayBuffer(param.texcoord0_buf_hdl);
            out_texcoord0             = texcoord0_buf.Load<float2>(primitive.texcoord0_start_idx + vertex_id);
        }

        // material_id
        out_material_id = primitive.material_idx;
    }
};

VsOutput main(
    uint vertex_id : SV_VertexID,    // 顶点 ID（自动由 GPU 提供）
    uint instance_id : SV_InstanceID // 实例 ID（自动由 GPU 提供）
#if POINT_SHADOW_MULTIVIEW
    , uint view_id : SV_ViewID
#endif
) {

    VsContext context;
#if !POINT_SHADOW_MULTIVIEW
    uint view_id = 0u;
#endif
    context.Load(vertex_id, instance_id, view_id);

    VsOutput output;

#if !SHADOW_DEPTH_PASS

    output.position       = context.out_clip_pos;
    output.normal         = context.out_normal;
    output.tangent        = context.out_tangent;
    output.texcoord0      = context.out_texcoord0;
    output.material_id    = context.out_material_id;
    output.primitive_id   = context.primitive_id;

#else

    output.position    = context.out_clip_pos;
    output.texcoord0   = context.out_texcoord0;
    output.material_id = context.out_material_id;

#endif

    return output;
}
