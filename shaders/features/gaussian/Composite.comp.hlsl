#include "shared/gaussian/GaussianRasterParameters.h"
[[vk::push_constant]] ConstantBuffer<Moer::GaussianDispatchParams> param;
[[vk::binding(0,0)]] Texture2D<float4> scene_color;
[[vk::binding(1,0)]] Texture2D<float4> gaussian_color;
[[vk::binding(2,0)]] Texture2D<float> transmittance;
[[vk::binding(3,0)]] StructuredBuffer<uint4> status;
[[vk::binding(4,0)]] StructuredBuffer<uint> ranges;
[[vk::binding(5,0)]] RWTexture2D<float4> output_color;
[numthreads(16,16,1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= param.info.xy)) return;
    float t = transmittance[id.xy];
    float3 result = gaussian_color[id.xy].rgb + t * scene_color[id.xy].rgb;
    if (param.job.x == 1) result = t.xxx;
    if (param.job.x == 2) {
        uint tile = id.x / 16 + (id.y / 16) * param.info.z;
        float density = saturate(log2(1.0 + float(ranges[tile*2+1] - ranges[tile*2])) / 12.0);
        result = float3(density, density*density, 1-density);
    }
    if (status[0].y != 0) result = float3(1,0,1);
    output_color[id.xy] = float4(result,1);
}
