/**
  * FXAA implementation
  * Reference: https://github.com/YXHXianYu/BJTU-Game-Engine/blob/main/engine/shader/glsl/final_fxaa.frag
  *            & https://zhuanlan.zhihu.com/p/431384101
  */

#include "framework/Bindless.hlsl"
#include "framework/Common.hlsl"
BINDLESS_BINDINGS(3, 2, 4, 5)

struct Constant {
    uint input_image;
    uint fxaa_mode;
    float2 resolution;
    float2 inv_resolution;
};

static const float fxaa_contrast_threshold = 0.025;
static const uint fxaa_search_limit = 8;

static const float Epsilon = 0.0001; // same with PBRMaterialFrag.hlsl

[[vk::push_constant]] ConstantBuffer<Constant> param;

float get_luminance(float2 uv) {
    return TextureHandle(param.input_image).Sample2D<float4>(uv).a;
}

float3 get_color(float2 uv) {
    return TextureHandle(param.input_image).Sample2D<float4>(uv).rgb;
}

float3 fxaa(float2 uv, float3 color, float M) {

    // 1. check contrast
    float S = get_luminance(uv + float2(0.0, -param.inv_resolution.y));
    float N = get_luminance(uv + float2(0.0, param.inv_resolution.y));
    float W = get_luminance(uv + float2(-param.inv_resolution.x, 0.0));
    float E = get_luminance(uv + float2(param.inv_resolution.x, 0.0));

    float max_luminance = max(M, max(S, max(N, max(W, E))));
    float min_luminance = min(M, min(S, min(N, min(W, E))));
    float contrast = max_luminance - min_luminance;

    if (contrast <= fxaa_contrast_threshold) return color;

    // 2. get luminance
    float NE = get_luminance(uv + param.inv_resolution);
    float NW = get_luminance(uv + float2(-param.inv_resolution.x, param.inv_resolution.y));
    float SW = get_luminance(uv - param.inv_resolution);
    float SE = get_luminance(uv + float2(param.inv_resolution.x, -param.inv_resolution.y));

    float blend = abs((2.0 * (N + E + S + W) + NE + NW + SE + SW) / 12.0 - M) / contrast;
    blend = smoothstep(0.0, 1.0, blend);
    blend = blend * blend;

    // 3. edge direction
    float vertical = 2.0 * abs(N+S-2.0*M) + abs(NE+SE-2.0*E) + abs(NW+SW-2.0*W);
    float horizontal = 2.0 * abs(E+W-2.0*M) + abs(NE+NW-2.0*W) + abs(SE+SW-2.0*S);

    bool is_edge_horizontal = vertical > horizontal;

    float2 pixel_step;
    if (is_edge_horizontal) {
        if (abs(N - M) > abs(S - M)) {
            pixel_step = float2(0.0, param.inv_resolution.y);
        } else {
            pixel_step = float2(0.0, -param.inv_resolution.y);
        }
    } else {
        if (abs(E - M) > abs(W - M)) {
            pixel_step = float2(param.inv_resolution.x, 0.0);
        } else {
            pixel_step = float2(-param.inv_resolution.x, 0.0);
        }
    }

    // 4. blend
    if (param.fxaa_mode == 1) {
        return get_color(uv + pixel_step * blend);
    }
    // assert param.fxaa_mode == 2
    // Important: the following code may has some bugs, so the default fxaa_mode is 1

    float positive = abs((is_edge_horizontal ? N : E) - M);
    float negative = abs((is_edge_horizontal ? S : W) - M);
    float gradient, opposite_luminance;
    if (positive > negative) {
        gradient = positive;
        opposite_luminance = is_edge_horizontal ? N : E;
    } else {
        gradient = negative;
        opposite_luminance = is_edge_horizontal ? S : W;
    }

    float2 uv_in_edge = uv + pixel_step * 0.5;
    float2 edge_step = is_edge_horizontal ? float2(param.inv_resolution.x, 0.0) : float2(0.0, param.inv_resolution.y);

    float edge_luminance = (M + opposite_luminance) * 0.5;
    float gradient_threshold = edge_luminance * 0.25;
    float p_luminance_delta;
    float n_luminance_delta;
    float p_distance = fxaa_search_limit;
    float n_distance = fxaa_search_limit;
    float i;
    
    // positive
    for (i = 1.0; i <= fxaa_search_limit; i += 1.0) {
        p_luminance_delta = get_luminance(uv_in_edge + i * edge_step) - edge_luminance;
        if (abs(p_luminance_delta) > gradient_threshold) {
            p_distance = i;
            break;
        }
    }
    // negative
    for (i = 1.0; i <= fxaa_search_limit; i += 1.0) {
        n_luminance_delta = get_luminance(uv_in_edge - i * edge_step) - edge_luminance;
        if (abs(n_luminance_delta) > gradient_threshold) {
            n_distance = i;
            break;
        }
    }
    // edge blend
    float edge_blend;
    if (p_distance < n_distance) {
        if (sign(p_luminance_delta) == sign(M - edge_luminance)) {
            edge_blend = 0.0;
        } else {
            edge_blend = 0.5 - p_distance / (p_distance + n_distance);
        }
    } else {
        if (sign(n_luminance_delta) == sign(M - edge_luminance)) {
            edge_blend = 0.0;
        } else {
            edge_blend = 0.5 - n_distance / (p_distance + n_distance);
        }
    }

    float final_blend = max(blend, edge_blend);

    return get_color(uv + pixel_step * final_blend);
}

float4 main(float2 in_uv : TEXCOORD0) : SV_TARGET {

    float4 input_image = TextureHandle(param.input_image).Sample2D<float4>(in_uv);
    float3 color = input_image.rgb;
    float M = input_image.a; // luminance
    
    if (param.fxaa_mode >= 1) {
        color = fxaa(in_uv, color, M);
    }

    return float4(color, 1.0);
}