#include "core/common/Bindless.hlsl"
BINDLESS_BINDINGS(4,5,6,7)
#include "shared/gaussian/GaussianShadowParameters.h"
[[vk::binding(0,0)]] ConstantBuffer<Moer::GaussianShadowData> data;
[[vk::binding(1,0)]] RWTexture2D<float4> output;
[numthreads(8,8,1)]
void main(uint3 id:SV_DispatchThreadID) {
    uint width,height; output.GetDimensions(width,height);
    if (id.x>=width || id.y>=height) return;
    uint columns=data.config.x==2?3:2;
    uint size=width/columns;
    uint view=id.x/size+(id.y/size)*columns;
    float depth=0;
    float2 uv=(float2(id.xy%size)+.5)/size;
    if (data.config.w != 0 && view<(data.config.x==2?6:data.config.y)) {
        if (data.config.x==2) {
            float2 p=float2(2*uv.x-1,1-2*uv.y);
            float3 dir;
            if(view==0) dir=float3(1,p.y,-p.x);
            else if(view==1) dir=float3(-1,p.y,p.x);
            else if(view==2) dir=float3(p.x,1,-p.y);
            else if(view==3) dir=float3(p.x,-1,p.y);
            else if(view==4) dir=float3(p.x,p.y,1);
            else dir=float3(-p.x,p.y,-1);
            depth=TextureHandle(data.gaussian_maps.x).SampleLevelCube<float>(dir,0);
        } else depth=TextureHandle(data.gaussian_maps[view]).SampleLevel<float>(uv,0);
    }
    output[id.xy]=float4(depth,depth,depth,1);
}
