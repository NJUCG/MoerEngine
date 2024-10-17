
struct VSInput {
    float3 Position : POSITION;
    float3 normal : NORMAL;
    float3 tangent : TANGENT;
    float2 UV : TEXCOORD0;
    int InstanceID : INSTANCEID;
};

struct VertexOutput {
    float4 Position : SV_POSITION;
    float2 UV : TEXCOORD0;
    float3 normal : NORMAL;
    int InstanceID : SV_InstanceID;
};

struct Constsant {
    float4 color;
    uint texture;
    uint buffer;
    float4x4 camera_view_proj;
};
[[vk::push_constant]] ConstantBuffer<Constsant> param;

VertexOutput main(VSInput input) {
    VertexOutput output;
    output.Position = float4(input.Position , 1.0f);
    output.Position = mul(param.camera_view_proj, output.Position);
    output.UV       = input.UV;
    output.normal   = input.normal;
    output.InstanceID = input.InstanceID;
    return output;
}