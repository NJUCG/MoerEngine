#include "shared/gaussian/GaussianRasterParameters.h"
[[vk::push_constant]] ConstantBuffer<Moer::GaussianDispatchParams> param;
[[vk::binding(0, 0)]] ConstantBuffer<Moer::GGaussianCamera> camera;
[[vk::binding(1, 0)]] StructuredBuffer<Moer::GProjectedGaussian> projected;
[[vk::binding(2, 0)]] StructuredBuffer<uint4> pairs;
[[vk::binding(3, 0)]] StructuredBuffer<uint> ranges;
[[vk::binding(4, 0)]] StructuredBuffer<uint4> status;
[[vk::binding(5, 0)]] RWTexture2D<float4> color_output;
[[vk::binding(6, 0)]] RWTexture2D<float> transmittance_output;
[[vk::binding(7, 0)]] RWTexture2D<float4> display_output;

float3 SRGBToLinear(float3 color) {
    return select(color <= 0.04045, color / 12.92, pow((color + 0.055) / 1.055, 2.4));
}

[numthreads(16,16,1)]
void main(uint3 group_id : SV_GroupID, uint3 local_id : SV_GroupThreadID) {
    uint2 pixel = group_id.xy * 16 + local_id.xy;
    if (any(pixel >= camera.viewport.xy)) return;
    if (status[0].y != 0) {
        color_output[pixel] = float4(1,0,1,0);
        transmittance_output[pixel] = 1;
        display_output[pixel] = float4(1,0,1,1);
        return;
    }
    uint tile_id = group_id.x + group_id.y * camera.viewport.z;
    uint2 range = uint2(ranges[tile_id * 2], ranges[tile_id * 2 + 1]);
    float3 color = 0;
    float transmittance = 1;
    for (uint pair_id = range.x; pair_id < range.y; ++pair_id) {
        Moer::GProjectedGaussian gaussian = projected[pairs[pair_id].z];
        float2 delta = gaussian.center_depth.xy - float2(pixel);
        float3 conic = gaussian.conic_opacity.xyz;
        float power = -0.5 * (conic.x * delta.x * delta.x + conic.z * delta.y * delta.y) - conic.y * delta.x * delta.y;
        if (power > 0) continue;
        float alpha = min(0.99, gaussian.conic_opacity.w * exp(power));
        if (alpha < camera.filter.y) continue;
        float next_transmittance = transmittance * (1 - alpha);
        if (next_transmittance < 0.0001) break;
        color += gaussian.color.xyz * (alpha * transmittance);
        transmittance = next_transmittance;
    }
    color_output[pixel] = float4(color, 1-transmittance);
    transmittance_output[pixel] = transmittance;
    // Standard 3DGS is trained against display RGB. Decode for Moer's hardware-sRGB UI target;
    // raw output retains the reference RGB values and separate transmittance.
    float3 composite = max(color + transmittance * camera.background.xyz, 0.0);
    float3 display = SRGBToLinear(saturate(composite));
    if (param.job.x == 1) {
        float3 linear_color = SRGBToLinear(composite) * exp2(camera.background.w);
        display = linear_color / (1 + linear_color);
    }
    if (param.job.y == 1) display = transmittance.xxx;
    if (param.job.y == 2) {
        float density = saturate(log2(1.0 + float(range.y - range.x)) / 12.0);
        display = float3(density, density*density, 1-density);
    }
    display_output[pixel] = float4(display, 1);
}
