#include "core/common/Bindless.hlsl"
BINDLESS_BINDINGS(4,5,6,7)
#include "features/gaussian/ShadowSampling.hlsli"
struct QueryParameters { uint4 info; };
[[vk::push_constant]] ConstantBuffer<QueryParameters> param;
[[vk::binding(0,0)]] StructuredBuffer<Moer::GaussianShadowData> typed_views;
[[vk::binding(1,0)]] RWStructuredBuffer<float4> output;
[[vk::binding(2,0)]] StructuredBuffer<float4> queries;
[numthreads(1,1,1)]
void main(uint3 id:SV_DispatchThreadID) {
    if(id.x >= param.info.y) return;
    Moer::GaussianShadowData raw = LoadGaussianShadowData(param.info.x);
    Moer::GaussianShadowData typed = typed_views[0];
    float4 world = queries[id.x];
    output[id.x*3] = mul(raw.world_to_clip[id.x%6],world);
    output[id.x*3+1] = mul(raw.world_to_view,world);
    output[id.x*3+2] = float4(GaussianShadowVisibility(raw,raw.gaussian_maps,world.xyz),
        GaussianShadowVisibility(typed,typed.gaussian_maps,world.xyz),
        GaussianShadowVisibility(raw,raw.mesh_maps,world.xyz,true),1);
}
