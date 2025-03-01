#include "framework/Bindless.hlsl"
#include "framework/Common.hlsl"
#include <shared/ShaderParameters.h>
BINDLESS_BINDINGS(3, 2, 4, 5)
#include "shared/Geometry.h"
#include "shared/utils/Packing.h"

struct VSInput {
    [[vk::location(0)]] float3 Position : POSITION;
    [[vk::location(1)]] uint normal : NORMAL;
    [[vk::location(2)]] uint tangent : TANGENT;
    [[vk::location(3)]] float2 UV : TEXCOORD0;
};

struct VertexOutput {
    float4 Position : SV_POSITION;
    float3 world_positon : POSITION;
    float2 UV : TEXCOORD0;
    float3 normal : NORMAL;
    float3 tangent : TANGENT;
    int InstanceID : INSTANCEID;
};

struct Constsant {
    float4 color;
    uint texture;
    uint buffer;
    uint instance_data;
    uint geometry_data;
    uint geometry_instance_data;
    float4x4 camera_view_proj;
};
[[vk::push_constant]] ConstantBuffer<Constsant> param;
[[vk::binding(0, 0)]] ConstantBuffer<Moer::ViewParam> constants : register(b0);

VertexOutput main(VSInput input, uint instance_id : SV_InstanceID) {

    ArrayBuffer instance_data_array = ArrayBuffer(param.instance_data);

    ArrayBuffer geom_instance_array = ArrayBuffer(param.geometry_instance_data);

    Moer::GeometryInstance geom_instance =
        geom_instance_array.Load<Moer::GeometryInstance>(instance_id);

    Moer::InstanceData instance_data = Moer::LoadInstanceData(instance_data_array.GetByteAddressBuffer(), geom_instance.instance_idx * sizeof(Moer::InstanceData));


    float3x4 model3x4 = instance_data.model2world;
    float3x3 model = (float3x3)model3x4;

    float3 world_position = mul(model3x4, float4(input.Position, 1.0f)).xyz;
    // printf("world_position %f %f %f\n", world_position.x, world_position.y, world_position.z);
    // float4 pos = mul(float4(world_position, 1.0f), param.camera_view_proj);

    float4x4 vp = mul(constants.view2clip, constants.world2view);
    float4 pos = mul(vp, float4(world_position, 1.0f));
    // printf("pos %f %f %f %f\n", pos.x, pos.y, pos.z, pos.w);


    // printf("sizeof InstanceData %d\n", sizeof(Moer::InstanceData));
    // printf("mat3x4\n %f %f %f %f\n %f %f %f %f\n %f %f %f %f\n ", model3x4._11,
    //                model3x4._12, model3x4._13, model3x4._14, model3x4._21, model3x4._22,
    //                model3x4._23, model3x4._24, model3x4._31, model3x4._32, model3x4._33,
    //                model3x4._34);

    VertexOutput output;
    output.Position = pos;
    output.UV = input.UV;
    output.normal = normalize(mul(model, Moer::Unpack_RGB8_SNORM(input.normal)));
    output.tangent = normalize(mul(model, Moer::Unpack_RGB8_SNORM(input.tangent)));
    output.InstanceID = geom_instance.geom_idx;

    output.world_positon = world_position;
    return output;
}