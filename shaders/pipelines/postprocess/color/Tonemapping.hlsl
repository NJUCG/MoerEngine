#include "core/math/STL.hlsli"
#include "shared/raster/ShaderParameters.h"

[[vk::push_constant]] ConstantBuffer<Moer::TonemappingPipelineBindlessParam> param;

[[vk::binding(0, 0)]] Texture2D    input_image;
[[vk::binding(1, 0)]] Buffer<uint> exposure;
[[vk::binding(2, 0)]] Buffer<uint> histogram;

float3 ConvertToLDR(float3 hdr_color) {
    float lum = STL::Color::Luminance(hdr_color);
    if (lum <= 0.0f) { // assert lum >= 0
        return float3(0.0f, 0.0f, 0.0f);
    }

    float target_exposure = asfloat(exposure[0]);
    if (target_exposure <= 0.0f) { // assert target_exposure >= 0
        return float3(0.0f, 0.0f, 0.0f);
    }

    float s_lum = lum * param.exposure_ev / target_exposure;
    float white_point_inv_squared = 1.0f / (param.ae.max_adapted_luminance * param.ae.max_adapted_luminance);
    float m_lum = s_lum * (1.0f + s_lum * white_point_inv_squared) / (1.0f + s_lum);

    return hdr_color * m_lum / lum;
}

float3 AutoExposure(float3 hdr_color, float2 uv) {

    [branch] if (param.ae.debug_visualize == 1) {
        [branch] if (0.48f <= uv.y && uv.y < 0.52f) {

            // 可视化histogram
            float v =
                float(histogram[floor(uv.x * 256.0)]) / Moer::TONEMAPPING_HISTOGRAM_POINT_FRAC_MULTIPLIER;
            float v2 = v / 1000.0;

            return v2.xxx;
        }
        else if (0.52f <= uv.y && uv.y < 0.56f) {

            // 可视化exposure
            float v  = asfloat(exposure[0]);
            float v2 = log2(v) / 32.0 + 0.5;

            return v2.xxx;
        }
    }
    
    return ConvertToLDR(hdr_color);
}

float3 ManualExposure(float3 hdr_color) {
    hdr_color *= param.exposure_ev;

    float3 color = (param.reinhard_enabled ? hdr_color / (hdr_color + 1.0) : hdr_color);

    return color;
}

// 注：Gamma矫正使用硬件sRGB实现，不需要在Shader中手动进行Gamma矫正
float4 main(float2 uv : TEXCOORD0, float4 pos : SV_Position) : SV_TARGET {

    float3 hdr_color = input_image[floor(pos.xy)].rgb;

    float3 color = 0.0f;

    [branch] if (param.ae.enabled) {
        color = AutoExposure(hdr_color, uv);
    } else {
        color = ManualExposure(hdr_color);
    }

    return float4(color, 1.0f);
}