#include "core/common/Bindless.hlsl"
#include "core/common/Common.hlsl"
BINDLESS_BINDINGS(3, 2, 4, 5)
#include "shared/ShaderParameters.h"
#include "shared/raster/ShaderParameters.h"

[[vk::push_constant]] ConstantBuffer<Moer::AoCompositeParam> param;
[[vk::binding(0, 0)]] RWTexture2D<float4>                    rw_output;

[numthreads(8, 8, 1)] void main(uint2 pixel_pos : SV_DispatchThreadID) {
    if (pixel_pos.x >= uint(param.full_resolution.x) || pixel_pos.y >= uint(param.full_resolution.y))
        return;

    float2 uv = (float2(pixel_pos) + 0.5) * param.inv_full_resolution;
    float  ao = TextureHandle(param.ao_tex).SampleLevel<float>(uv);

    if (param.ao_mode == Moer::EAoMode::RTAO_AO_ONLY || param.ao_mode == Moer::EAoMode::SSAO_AO_ONLY ||
        param.ao_mode == Moer::EAoMode::SSDO_AO_ONLY) {
        rw_output[pixel_pos] = float4(ao, ao, ao, 1.0);
    } else {
        float3 color         = TextureHandle(param.color_tex).SampleLevel<float3>(uv);
        rw_output[pixel_pos] = float4(color * ao, 1.0);
    }
}
