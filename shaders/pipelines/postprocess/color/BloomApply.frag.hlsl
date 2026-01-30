#include "core/common/Bindless.hlsl"
#include "core/common/Common.hlsl"
BINDLESS_BINDINGS(3, 2, 4, 5)
#include "shared/raster/ShaderParameters.h"

[[vk::push_constant]] ConstantBuffer<Moer::BloomApplyBindlessParam> param;

float4 main(float2 uv : TEXCOORD0) : SV_TARGET {
    // 采样上采样链的第 0 层（最终结果）
    float3 bloom_color = TextureHandle(param.bloom_result_hdl).Sample2D<float4>(uv).rgb;

    return float4(bloom_color * param.bloom_intensity, 1.0);
}