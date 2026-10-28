#include "shared/gaussian/GaussianRasterParameters.h"
[[vk::push_constant]] ConstantBuffer<Moer::GaussianCompositeParams> param;
[[vk::binding(0,0)]] Texture2D<float4> scene_color;
[[vk::binding(1,0)]] Texture2D<float4> gaussian_color;
[[vk::binding(2,0)]] Texture2D<float> transmittance;
[[vk::binding(3,0)]] StructuredBuffer<uint4> status;
[[vk::binding(4,0)]] StructuredBuffer<uint> ranges;
[[vk::binding(5,0)]] RWTexture2D<float4> output_color;
float3 SRGBToLinear(float3 color) {
    color = max(color, 0);
    return select(color <= 0.04045, color / 12.92, pow((color + 0.055) / 1.055, 2.4));
}
[numthreads(16,16,1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= param.viewport.xy)) return;
    float t = transmittance[id.xy];
    float alpha = 1.0 - t;
    float3 premultiplied_rgb = gaussian_color[id.xy].rgb;
    float3 layer = alpha > 0 ? premultiplied_rgb / alpha : 0;
    // Decode straight RGB, then premultiply again: decoding premultiplied RGB
    // directly would darken transparent edges. Never decode HDR Mesh colors.
    float3 linear_layer = SRGBToLinear(layer) * param.brightness_gain;
    float3 result = alpha * linear_layer + t * scene_color[id.xy].rgb;
    if (param.is_visibility_debug != 0) result = premultiplied_rgb + t * scene_color[id.xy].rgb;
    if (param.viewport.w == 1) result = t.xxx;
    if (param.viewport.w == 2) {
        uint tile = id.x / 16 + (id.y / 16) * param.viewport.z;
        float density = saturate(log2(1.0 + float(ranges[tile*2+1] - ranges[tile*2])) / 12.0);
        result = float3(density, density*density, 1-density);
    }
    if (status[0].y != 0) result = float3(1,0,1);
    output_color[id.xy] = float4(result,1);
}
