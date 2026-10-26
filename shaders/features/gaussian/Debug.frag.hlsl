#include "shared/gaussian/GaussianRasterParameters.h"
[[vk::push_constant]] ConstantBuffer<Moer::GaussianDispatchParams> param;
[[vk::binding(0,0)]] Texture2D<float> transmittance;
[[vk::binding(1,0)]] StructuredBuffer<uint4> status;
[[vk::binding(2,0)]] StructuredBuffer<uint> ranges;
[[vk::binding(3,0)]] Texture2D<float4> debug_color;
float4 main(float4 position : SV_Position) : SV_TARGET {
    uint2 pixel = uint2(position.xy);
    float3 color = transmittance[pixel].xxx;
    if (param.job.x == 2) {
        uint tile = pixel.x/16 + (pixel.y/16)*param.info.z;
        float density = saturate(log2(1.0 + float(ranges[tile*2+1]-ranges[tile*2]))/12.0);
        color = float3(density,density*density,1-density);
    }
    if (param.job.x == 3) {
        float alpha = 1-transmittance[pixel];
        color = alpha > 1e-5 ? saturate(debug_color[pixel].rgb / alpha) : float3(1,1,1);
    } else if (param.job.x == 4) {
        color = 0;
        if (param.job.y != 0) {
            uint w,h; debug_color.GetDimensions(w,h);
            uint2 p = min(uint2(float2(pixel) / float2(param.info.xy) * float2(w,h)), uint2(w-1,h-1));
            color = debug_color[p].rgb;
        }
    }
    if(status[0].y != 0) color=float3(1,0,1);
    return float4(color,1);
}
