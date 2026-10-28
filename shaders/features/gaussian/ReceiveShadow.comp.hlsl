#include "core/common/Bindless.hlsl"
BINDLESS_BINDINGS(4,5,6,7)
#include "shared/gaussian/GaussianRasterParameters.h"
#include "features/gaussian/ShadowSampling.hlsli"
[[vk::push_constant]] ConstantBuffer<Moer::GaussianDispatchParams> param;
[[vk::binding(0,0)]] RWStructuredBuffer<Moer::GProjectedGaussian> projected;
[[vk::binding(1,0)]] StructuredBuffer<Moer::GGaussian> gaussians;
[[vk::binding(2,0)]] StructuredBuffer<Moer::GGaussianInstance> instances;
[[vk::binding(3,0)]] StructuredBuffer<Moer::GaussianShadowData> shadow_views;
[numthreads(256,1,1)]
void main(uint3 id:SV_DispatchThreadID) {
    if (id.x>=param.job.w) return;
    Moer::GGaussianInstance instance=instances[param.job.y];
    float3 world=mul(instance.world_transform,float4(gaussians[param.job.x+id.x].position.xyz,1)).xyz;
    Moer::GaussianShadowData data=shadow_views[0];
    float visibility=GaussianShadowVisibility(data,data.mesh_maps,world,true);
    projected[param.job.z+id.x].color.w=visibility;
}
