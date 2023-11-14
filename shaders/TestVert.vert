struct VSInput
{
[[vk::location(0)]] float3 Position : POSITION0;
[[vk::location(1)]] float3 Color : COLOR0;
};
Texture2D<float4> foo[5] : register(t2);
Buffer bar : register(t7);
RWBuffer<float4> dataLog : register(u1);
SamplerState samp[2] : register(s0);
SamplerState aniso : register(s3);

struct UBO
{
	float4x4 projectionMatrix;
	float4x4 modelMatrix;
	float4x4 viewMatrix;
};

[[vk::push_constant]]
ConstantBuffer<UBO> ubo : register(b0, space1);

struct VSOutput
{
	float4 Position : SV_POSITION;
[[vk::location(0)]] float3 Color : COLOR0;
};

VSOutput main(VSInput input, uint VertexIndex : SV_VertexID)
{
	VSOutput output = (VSOutput)0;
	output.Color = input.Color * float(VertexIndex);
	dataLog[0] = 1.f;
	float4 vShadowDepths;
	float2 uv = float2(0, 0);
    vShadowDepths.x = foo[0].SampleLevel(samp[0], uv, 0).r;
	output.Position = mul(ubo.projectionMatrix, mul(ubo.viewMatrix, mul(ubo.modelMatrix, float4(input.Position.xyz, 1.0))));
	return output;
}