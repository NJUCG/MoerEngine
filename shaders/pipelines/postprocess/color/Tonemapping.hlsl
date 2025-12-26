#include "core/math/STL.hlsli"
#include "shared/raster/ShaderParameters.h"

[[vk::push_constant]] ConstantBuffer<Moer::TonemappingPipelineBindlessParam> param;

[[vk::binding(0, 0)]] Texture2D    input_image;
[[vk::binding(1, 0)]] Buffer<uint> exposure;
[[vk::binding(2, 0)]] Buffer<uint> histogram;

// ACES 拟合函数
float3 ACESFilm(float3 x) {
    float a = 2.51f;
    float b = 0.03f;
    float c = 2.43f;
    float d = 0.59f;
    float e = 0.14f;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

// ACES 色调映射
float3 AE_ACESToneMapping(float3 hdr_color, float target_exposure, float exposure_ev) {
    // 1. 计算曝光缩放因子 (Exposure Scaling)
    // 你原来的逻辑：s_lum = lum * exposure_ev / target_exposure
    // 这里的 exposure_ev 通常是用户的曝光补偿（如 1.0），target_exposure 是 AE 算出来的平均亮度
    float exposure_factor = exposure_ev / target_exposure;

    // 2. 将 HDR 颜色映射到曝光后的线性空间
    float3 exposed_rgb = hdr_color * exposure_factor;

    // 3. 应用 ACES Filmic Tone Mapping
    // 注意：ACES 建议直接作用于 RGB，这样在高光处会有自然的色偏（如火光变黄），更好看
    return ACESFilm(exposed_rgb);
}

// 一种带白点修正的扩展 Reinhard 算法
float3 AE_DefaultToneMapping(float3 hdr_color, float target_exposure, float exposure_ev) {
    float lum = STL::Color::Luminance(hdr_color);
    if (lum <= 0.0f) { // assert lum >= 0
        return float3(0.0f, 0.0f, 0.0f);
    }

    float s_lum = lum * exposure_ev / target_exposure;
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

    float target_exposure = asfloat(exposure[0]);
    if (target_exposure <= 0.0f) { // assert target_exposure >= 0
        return float3(0.0f, 0.0f, 0.0f);
    }
    
    if (param.ae.aces_tonemapping_enabled) {
        return AE_ACESToneMapping(hdr_color, target_exposure, param.exposure_ev);
    } else {
        return AE_DefaultToneMapping(hdr_color, target_exposure, param.exposure_ev);
    }
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