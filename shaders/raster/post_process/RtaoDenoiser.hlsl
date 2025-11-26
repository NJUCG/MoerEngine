#include "framework/Bindless.hlsl"
#include "framework/Common.hlsl"
BINDLESS_BINDINGS(3, 2, 4, 5)
#include "shared/raster/ShaderParameters.h"

[[vk::push_constant]] ConstantBuffer<Moer::RtaoDenoiserPassBindlessParam> param;

struct RtaoDenoiserOutput {
    float accumulate_ao : SV_TARGET0;
    float4 color_with_ao : SV_TARGET1;
};

void get_reprojected_ao(float2 uv, float curr_ao, out float out_history_ao, out float out_history_weight) {

    float2 motion_vector = TextureHandle(param.motion_vector_tex).Sample2D<float2>(uv).rg; // value in [-1, 1] (NDC Space)
    float2 prev_uv = float2(uv.x - motion_vector.x, uv.y + motion_vector.y);
    if (prev_uv.x <= 0.0 || prev_uv.x >= 1.0 || prev_uv.y <= 0.0 || prev_uv.y >= 1.0) {
        prev_uv = uv;
        if (param.is_validation_enable) {
            out_history_weight = 0.0;
            return;
        }
    }

    out_history_ao = TextureHandle(param.history_ao_tex).Sample2D<float>(prev_uv);

    // validation
    if (param.is_validation_enable) {
        float curr_depth = TextureHandle(param.depth_tex).Sample2D<float>(uv);
        float prev_depth = TextureHandle(param.depth_tex).Sample2D<float>(prev_uv);
        float3 curr_normal = Raster::UnpackNormal(TextureHandle(param.normal_tex).Sample2D<float3>(uv));
        float3 prev_normal = Raster::UnpackNormal(TextureHandle(param.normal_tex).Sample2D<float3>(prev_uv));
        
        if (abs(curr_depth - prev_depth) > curr_depth * param.valid_depth_threshold) {
            out_history_ao = curr_ao;
        }
        if (dot(curr_normal, prev_normal) < param.valid_normal_threshold) {
            out_history_ao = curr_ao;
        }

        float mv_len_sqr = dot(motion_vector, motion_vector);

        out_history_weight *= mv_len_sqr < 0.001 ? 1.0 : sqrt(1.0 - sqrt(mv_len_sqr) * 0.5);
    }
}

RtaoDenoiserOutput main(float2 uv : TEXCOORD0) {
    RtaoDenoiserOutput output;
    /*
        param.history_ao_tex
        param.curr_ao_tex
        param.prev_ao_tex
        param.motion_vector_tex
        param.depth_tex
    */

    // current info
    float curr_ao = TextureHandle(param.curr_ao_tex).Sample2D<float>(uv);

    float history_ao = 0.0;
    float history_weight = param.history_ratio;

    // reprojection
    if (param.is_reprojection_enable) {
        get_reprojected_ao(uv, curr_ao, /* out */ history_ao, /* out */ history_weight);
    } else {
        history_ao = TextureHandle(param.history_ao_tex).Sample2D<float>(uv);
    }

    // get result
    output.accumulate_ao = history_ao * history_weight + curr_ao * (1.0 - history_weight);

    if (param.is_rtao_ao_only) {
        output.color_with_ao = output.accumulate_ao;
    } else {
        float4 color = TextureHandle(param.color_tex).Sample2D<float4>(uv);
        output.color_with_ao = float4(color.rgb * output.accumulate_ao, 1.0);
    }
    return output;
}