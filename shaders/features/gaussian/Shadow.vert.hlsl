#include "features/gaussian/GaussianMath.hlsli"
#include "shared/gaussian/GaussianShadowParameters.h"
[[vk::push_constant]] ConstantBuffer<Moer::GaussianShadowDraw> param;
[[vk::binding(0,0)]] StructuredBuffer<Moer::GGaussian> gaussians;
[[vk::binding(1,0)]] StructuredBuffer<Moer::GGaussianInstance> instances;
struct Output {
    float4 position : SV_Position;
    float2 delta : TEXCOORD0;
    nointerpolation float4 conic : TEXCOORD1;
};
Output main(uint vertex : SV_VertexID, uint id : SV_InstanceID) {
    Output o = (Output)0;
    o.position = float4(2,2,0,1);
    Moer::GGaussian g = gaussians[param.info.x + id];
    Moer::GGaussianInstance instance = instances[param.info.y];
    float3 world = mul(instance.world_transform,float4(g.position.xyz,1)).xyz;
    float4 clip = mul(param.world_to_clip,float4(world,1));
    if (!all(isfinite(clip)) || clip.w <= 1e-6 || clip.z < 0 || clip.z > clip.w) return o;
    float3x3 axes = mul((float3x3)instance.world_transform,GaussianRotation(g.rotation));
    axes[0] *= g.scale_opacity.xyz; axes[1] *= g.scale_opacity.xyz; axes[2] *= g.scale_opacity.xyz;
    float3 jx = .5*param.info.z*(param.world_to_clip[0].xyz*clip.w-clip.x*param.world_to_clip[3].xyz)/(clip.w*clip.w);
    float3 jy = -.5*param.info.z*(param.world_to_clip[1].xyz*clip.w-clip.y*param.world_to_clip[3].xyz)/(clip.w*clip.w);
    float3 ax = mul(jx,axes), ay = mul(jy,axes);
    float a=dot(ax,ax)+.3, b=dot(ax,ay), c=dot(ay,ay)+.3;
    float determinant = a*c-b*b;
    float opacity = min(.99,g.scale_opacity.w*instance.color_opacity.w);
    if (determinant <= 0 || opacity < param.options.x) return o;
    float radius = sqrt(2*log(opacity/param.options.x)*max(a,c));
    static const float2 corners[6] = {float2(-1,-1),float2(1,-1),float2(-1,1),float2(-1,1),float2(1,-1),float2(1,1)};
    o.delta = corners[vertex]*radius;
    o.position = float4(clip.xy/clip.w + o.delta*float2(2,-2)/param.info.z,clip.z/clip.w,1);
    o.conic = float4(c,-b,a,opacity)/float4(determinant.xxx,1);
    return o;
}
