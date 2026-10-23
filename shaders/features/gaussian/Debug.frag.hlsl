#include "shared/gaussian/GaussianRasterParameters.h"
[[vk::push_constant]] ConstantBuffer<Moer::GaussianDispatchParams> param;
[[vk::binding(0,0)]] Texture2D<float> transmittance;
[[vk::binding(1,0)]] StructuredBuffer<uint4> status;
[[vk::binding(2,0)]] StructuredBuffer<uint> ranges;
float4 main(float4 position : SV_Position) : SV_TARGET {
    uint2 pixel = uint2(position.xy);
    float3 color = transmittance[pixel].xxx;
    if (param.job.x == 2) {
        uint tile = pixel.x/16 + (pixel.y/16)*param.info.z;
        float density = saturate(log2(1.0 + float(ranges[tile*2+1]-ranges[tile*2]))/12.0);
        color = float3(density,density*density,1-density);
    }
    if(status[0].y != 0) color=float3(1,0,1);
    return float4(color,1);
}
