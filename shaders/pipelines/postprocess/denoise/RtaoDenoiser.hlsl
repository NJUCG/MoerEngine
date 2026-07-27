#include "core/common/Bindless.hlsl"
#include "core/common/Common.hlsl"
BINDLESS_BINDINGS(3, 2, 4, 5)
#include "shared/raster/ShaderParameters.h"

[[vk::push_constant]] ConstantBuffer<Moer::RtaoDenoiserPassBindlessParam> param;

struct RtaoDenoiserOutput {
    float accumulate_ao : SV_TARGET0;
};

float2 get_motion_vector_uv(float2 motion_vector_ndc) {
    return float2(motion_vector_ndc.x, -motion_vector_ndc.y) * 0.5;
}

void get_curr_ao_bounds(float2 uv, out float out_min_ao, out float out_max_ao) {
    out_min_ao = 1.0;
    out_max_ao = 0.0;

    [unroll] for (int y = -1; y <= 1; y++) {
        [unroll] for (int x = -1; x <= 1; x++) {
            float2 sample_uv = saturate(uv + float2(x, y) * param.inv_resolution);
            float  sample_ao = TextureHandle(param.curr_ao_tex).Sample2D<float>(sample_uv);
            out_min_ao       = min(out_min_ao, sample_ao);
            out_max_ao       = max(out_max_ao, sample_ao);
        }
    }
}

bool is_history_valid(float2 uv, float2 prev_uv) {
    float  curr_depth = TextureHandle(param.depth_tex).Sample2D<float>(uv);
    float  prev_depth = TextureHandle(param.depth_tex).Sample2D<float>(prev_uv);
    float3 curr_normal = Raster::UnpackNormal(TextureHandle(param.normal_tex).Sample2D<float3>(uv));
    float3 prev_normal = Raster::UnpackNormal(TextureHandle(param.normal_tex).Sample2D<float3>(prev_uv));

    float depth_scale = max(max(curr_depth, prev_depth), 0.1);
    if (abs(curr_depth - prev_depth) > depth_scale * param.valid_depth_threshold) {
        return false;
    }

    return dot(curr_normal, prev_normal) >= param.valid_normal_threshold;
}

float get_motion_history_scale(float2 motion_vector_uv) {
    float2 safe_inv_resolution = max(param.inv_resolution, float2(1e-6, 1e-6));
    float2 motion_in_pixels    = abs(motion_vector_uv / safe_inv_resolution);
    return exp2(-0.35 * length(motion_in_pixels));
}

void get_reprojected_ao(float2 uv, float curr_ao, out float out_history_ao, out float out_history_weight) {
    out_history_ao     = curr_ao;
    out_history_weight = param.history_ratio;

    float2 motion_vector_ndc =
        TextureHandle(param.motion_vector_tex).Sample2D<float2>(uv).rg; // value in [-1, 1] (NDC Space)
    float2 motion_vector_uv = get_motion_vector_uv(motion_vector_ndc);
    float2 prev_uv          = uv - motion_vector_uv;

    if (prev_uv.x <= 0.0 || prev_uv.x >= 1.0 || prev_uv.y <= 0.0 || prev_uv.y >= 1.0) {
        out_history_weight = 0.0;
        return;
    }

    out_history_ao = TextureHandle(param.history_ao_tex).Sample2D<float>(prev_uv);

    if (param.is_validation_enable && !is_history_valid(uv, prev_uv)) {
        out_history_weight = 0.0;
        out_history_ao     = curr_ao;
        return;
    }

    if (param.is_history_clamp_enable != 0) {
        float neighborhood_min_ao;
        float neighborhood_max_ao;
        get_curr_ao_bounds(uv, neighborhood_min_ao, neighborhood_max_ao);

        float neighborhood_pad = max(0.02, (neighborhood_max_ao - neighborhood_min_ao) * 0.25);
        out_history_ao         = clamp(
            out_history_ao,
            max(0.0, neighborhood_min_ao - neighborhood_pad),
            min(1.0, neighborhood_max_ao + neighborhood_pad)
        );
    }

    if (param.is_motion_weighting_enable != 0) {
        out_history_weight *= get_motion_history_scale(motion_vector_uv);
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

    float history_ao     = 0.0;
    float history_weight = param.history_ratio;

    // reprojection
    if (param.is_reprojection_enable) {
        get_reprojected_ao(uv, curr_ao, /* out */ history_ao, /* out */ history_weight);
    } else {
        history_ao = TextureHandle(param.history_ao_tex).Sample2D<float>(uv);
    }

    output.accumulate_ao = history_ao * history_weight + curr_ao * (1.0 - history_weight);
    return output;
}
