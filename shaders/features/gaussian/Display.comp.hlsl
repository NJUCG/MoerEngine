#include "shared/gaussian/GaussianRasterParameters.h"
[[vk::push_constant]] ConstantBuffer<Moer::GaussianDispatchParams> param;
[[vk::binding(0, 0)]] Texture2D<float4> color_input;
[[vk::binding(1, 0)]] Texture2D<float> transmittance_input;
[[vk::binding(2, 0)]] StructuredBuffer<uint> ranges;
[[vk::binding(3, 0)]] StructuredBuffer<uint4> status;
[[vk::binding(4, 0)]] RWTexture2D<float4> display_output;
[[vk::binding(5, 0)]] ConstantBuffer<Moer::GGaussianCamera> camera;
float3 SRGBToLinear(float3 color) {
    return select(color <= 0.04045, color / 12.92, pow((color + 0.055) / 1.055, 2.4));
}
[numthreads(16,16,1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= camera.viewport.xy)) return;
    float transmittance = transmittance_input[id.xy];
    float3 composite = max(color_input[id.xy].rgb + transmittance * camera.background.xyz, 0.0);
    float3 display = SRGBToLinear(saturate(composite));
    if (param.job.x == 1) {
        float3 linear_color = SRGBToLinear(composite) * exp2(camera.background.w);
        display = linear_color / (1 + linear_color);
    }
    if (param.job.y == 1) display = transmittance.xxx;
    if (param.job.y == 2) {
        uint tile_id = id.x / 16 + (id.y / 16) * camera.viewport.z;
        float density = saturate(log2(1.0 + float(ranges[tile_id * 2 + 1] - ranges[tile_id * 2])) / 12.0);
        display = float3(density, density*density, 1-density);
    }
    if (status[0].y != 0) display = float3(1,0,1);
    display_output[id.xy] = float4(display, 1);
}
