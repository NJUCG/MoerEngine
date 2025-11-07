#include "framework/Bindless.hlsl"
#include "framework/Common.hlsl"
BINDLESS_BINDINGS(3, 2, 4, 5)
#include "shared/raster/post_process/ShaderParameters.h"

[[vk::push_constant]] ConstantBuffer<Moer::RtaoDenoiserPassBindlessParam> param;

struct RtaoDenoiserOutput {
    float accumulate_ao : SV_TARGET0;
    float4 color_with_ao : SV_TARGET1;
};

RtaoDenoiserOutput main(float2 uv : TEXCOORD0) {
    RtaoDenoiserOutput output;
    /*
        param.history_ao_tex
        param.curr_ao_tex
        param.prev_ao_tex
        param.motion_vector_tex
        param.depth_tex
    */

    float history_ao = TextureHandle(param.history_ao_tex).Sample2D<float>(uv);
    float curr_ao    = TextureHandle(param.curr_ao_tex).Sample2D<float>(uv);
    // float prev_ao    = TextureHandle(param.prev_ao_tex).Sample2D<float>(uv);
    // float2 motion_vector = TextureHandle(param.motion_vector_tex).Sample2D<float2>(uv).rg;
    // float depth = TextureHandle(param.depth_tex).Sample2D<float>(uv);

    output.accumulate_ao = history_ao * param.history_ratio + curr_ao * (1.0 - param.history_ratio);

    if (param.is_rtao_ao_only) {
        output.color_with_ao = output.accumulate_ao;
    } else {
        float4 color = TextureHandle(param.color_tex).Sample2D<float4>(uv);
        output.color_with_ao = float4(color.rgb * output.accumulate_ao, 1.0);
    }
    return output;
}