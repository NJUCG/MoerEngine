#include "core/common/Bindless.hlsl"
#include "core/common/Common.hlsl"
BINDLESS_BINDINGS(3, 2, 4, 5)
#include "shared/raster/ShaderParameters.h"
#include "pipelines/postprocess/lighting_effects/AoHistory.hlsli"

DEFINE_SHADER_CONSTANT(Moer::RtaoDenoiserPassBindlessParam, param)

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
            float2 sample_uv = saturate(uv + float2(x, y) * moer_constants.param.inv_resolution);
            float  sample_ao = TextureHandle(moer_constants.param.curr_ao_tex).Sample2D<float>(sample_uv);
            out_min_ao       = min(out_min_ao, sample_ao);
            out_max_ao       = max(out_max_ao, sample_ao);
        }
    }
}

float ReadNeighborFootprint(int2 pixel, uint2 size, uint4 current) {
    if (any(pixel < 0) || any(pixel >= int2(size))) {
        return 1e30;
    }
    uint4 neighbor = TextureHandle(moer_constants.param.current_geometry_tex).GetTexture2D<uint4>().Load(int3(pixel, 0));
    return AoNeighborFootprint(current, neighbor, moer_constants.param.valid_normal_threshold);
}

float GetPixelFootprint(int2 pixel, uint2 size, uint4 current) {
    float horizontal = min(ReadNeighborFootprint(pixel + int2(-1, 0), size, current),
                           ReadNeighborFootprint(pixel + int2(1, 0), size, current));
    float vertical = min(ReadNeighborFootprint(pixel + int2(0, -1), size, current),
                         ReadNeighborFootprint(pixel + int2(0, 1), size, current));
    return max(horizontal < 1e29 ? horizontal : 0.0, vertical < 1e29 ? vertical : 0.0);
}

float get_motion_history_scale(float2 motion_vector_uv) {
    float2 safe_inv_resolution = max(moer_constants.param.inv_resolution, float2(1e-6, 1e-6));
    float2 motion_in_pixels    = abs(motion_vector_uv / safe_inv_resolution);
    return exp2(-0.35 * length(motion_in_pixels));
}

RtaoDenoiserOutput main(float2 uv : TEXCOORD0) {
    RtaoDenoiserOutput output;
    float curr_ao = TextureHandle(moer_constants.param.curr_ao_tex).Sample2D<float>(uv);
    output.accumulate_ao = curr_ao;

    // No fetch from uninitialized histories on the first accepted frame or after reset/mode changes.
    if (moer_constants.param.history_ratio <= 0.0) {
        return output;
    }

    uint width, height;
    TextureHandle(moer_constants.param.current_geometry_tex).GetTexture2D<uint4>().GetDimensions(width, height);
    uint2 size = uint2(width, height);
    int2 pixel = min(int2(uv * size), int2(size) - 1);
    uint4 current = TextureHandle(moer_constants.param.current_geometry_tex).GetTexture2D<uint4>().Load(int3(pixel, 0));
    if (!IsAoGeometryValid(current)) {
        return output;
    }

    float2 motion_vector_uv = float2(0, 0);
    if (moer_constants.param.is_reprojection_enable != 0) {
        motion_vector_uv = get_motion_vector_uv(TextureHandle(moer_constants.param.motion_vector_tex).Sample2D<float2>(uv));
    }
    float2 prev_uv = uv - motion_vector_uv;
    if (!all(isfinite(prev_uv)) || any(prev_uv <= 0.0) || any(prev_uv >= 1.0)) {
        return output;
    }

    // AO and its geometry are sampled from the very same history texel and accepted generation.
    int2 history_pixel = min(int2(prev_uv * size), int2(size) - 1);
    uint4 history = TextureHandle(moer_constants.param.history_geometry_tex).GetTexture2D<uint4>().Load(int3(history_pixel, 0));
    if (!IsAoGeometryValid(history)) {
        return output;
    }
    if (moer_constants.param.is_validation_enable != 0 && !IsAoHistoryGeometryValid(
            current, history, moer_constants.param.camera_position,
            moer_constants.param.valid_position_threshold, moer_constants.param.valid_normal_threshold,
            GetPixelFootprint(pixel, size, current))) {
        return output;
    }

    float history_ao = TextureHandle(moer_constants.param.history_ao_tex).GetTexture2D<float>().Load(int3(history_pixel, 0));
    if (!isfinite(history_ao)) {
        return output;
    }
    if (moer_constants.param.is_history_clamp_enable != 0) {
        float neighborhood_min_ao;
        float neighborhood_max_ao;
        get_curr_ao_bounds(uv, neighborhood_min_ao, neighborhood_max_ao);
        float neighborhood_pad = max(0.02, (neighborhood_max_ao - neighborhood_min_ao) * 0.25);
        history_ao = clamp(history_ao, max(0.0, neighborhood_min_ao - neighborhood_pad),
                           min(1.0, neighborhood_max_ao + neighborhood_pad));
    }

    float history_weight = moer_constants.param.history_ratio;
    if (moer_constants.param.is_reprojection_enable != 0 && moer_constants.param.is_motion_weighting_enable != 0) {
        history_weight *= get_motion_history_scale(motion_vector_uv);
    }
    output.accumulate_ao = history_ao * history_weight + curr_ao * (1.0 - history_weight);
    return output;
}
