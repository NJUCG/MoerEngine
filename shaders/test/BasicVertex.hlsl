
struct VSInput {
  float3 Position : POSITION;
  float2 UV : TEXCOORD0;
};

struct VertexOutput {
  float4 Position : SV_POSITION;
  float2 UV : TEXCOORD0;
};

VertexOutput main(VSInput input) {
    VertexOutput output;
    output.Position = float4(input.Position, 1.0f);
    output.UV = input.UV;
    return output;
}