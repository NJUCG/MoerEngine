#include "core/common/Bindless.hlsl"
#include "core/common/Common.hlsl"
BINDLESS_BINDINGS(3, 2, 4, 5)
#include "shared/ShaderParameters.h"
#include "shared/raster/ShaderParameters.h"
#include "pipelines/postprocess/lighting_effects/AoGeometry.hlsli"

DEFINE_SHADER_CONSTANT(Moer::AoCompositeParam, param)
[[vk::binding(0, 0)]] RWTexture2D<float4>                    rw_output;

// Joint Bilateral Upsample：用全分辨率深度+法线引导，对半分辨率AO进行边缘保持上采样
float4 SampleAoLighting(float2 uv, bool include_indirect) {
    float ao = TextureHandle(moer_constants.param.ao_tex).SampleLevel<float>(uv);
    float3 indirect = include_indirect ? TextureHandle(moer_constants.param.indirect_tex).SampleLevel<float3>(uv) : float3(0, 0, 0);
    return float4(indirect, ao);
}

float4 JointBilateralUpsampleAO(float2 uv, bool include_indirect) {
    float2 ao_res     = moer_constants.param.ao_resolution;
    float2 inv_ao_res = 1.0 / ao_res;

    // 全分辨率像素处的深度和法线
    AoSurface center = LoadAoSurface(uv, moer_constants.param.depth_tex, moer_constants.param.normal_tex);

    // 低分辨率纹理空间中的亚像素位置
    float2 low_pos    = uv * ao_res - 0.5;
    int2   base_texel = int2(floor(low_pos));
    float2 frac_pos   = frac(low_pos);

    static const float kDepthScale  = 100.0; // 深度差异灵敏度（相对深度）
    static const float kNormalPower = 32.0;  // 法线差异灵敏度

    float4 lighting_sum = 0.0;
    float w_sum  = 0.0;

    [unroll]
    for (int y = 0; y <= 1; y++) {
        [unroll]
        for (int x = 0; x <= 1; x++) {
            int2   tap    = base_texel + int2(x, y);
            float2 tap_uv = (float2(tap) + 0.5) * inv_ao_res;

            float4 lighting_sample = SampleAoLighting(tap_uv, include_indirect);

            // 在低分辨率像素中心位置采样全分辨率深度和法线（用于边缘判断）
            AoSurface sample_surface = LoadAoSurface(tap_uv, moer_constants.param.depth_tex, moer_constants.param.normal_tex);

            // 空间权重（双线性插值权重）
            float w_spatial = (x == 0 ? (1.0 - frac_pos.x) : frac_pos.x)
                            * (y == 0 ? (1.0 - frac_pos.y) : frac_pos.y);

            // 深度权重：相对深度差越大，权重越低
            float depth_diff = abs(center.depth - sample_surface.depth) / max(center.depth, 1e-6);
            float w_depth    = exp(-depth_diff * kDepthScale);

            // 法线权重：法线夹角越大，权重越低
            float w_normal = pow(max(dot(center.normal, sample_surface.normal), 0.0), kNormalPower);

            float w = w_spatial * w_depth * w_normal;
            lighting_sum += lighting_sample * w;
            w_sum  += w;
        }
    }

    // 所有权重都很小时回退到双线性采样（天空等无几何区域）
    return w_sum > 1e-6 ? lighting_sum / w_sum : SampleAoLighting(uv, include_indirect);
}

[numthreads(8, 8, 1)] void main(uint2 pixel_pos : SV_DispatchThreadID) {
    if (pixel_pos.x >= uint(moer_constants.param.full_resolution.x) || pixel_pos.y >= uint(moer_constants.param.full_resolution.y))
        return;

    float2 uv = (float2(pixel_pos) + 0.5) * moer_constants.param.inv_full_resolution;

    const bool include_indirect = moer_constants.param.ao_mode == Moer::EAoMode::SSDO;
    float4 lighting;
    if (moer_constants.param.is_half_resolution) {
        lighting = JointBilateralUpsampleAO(uv, include_indirect);
    } else {
        lighting = SampleAoLighting(uv, include_indirect);
    }

    if (moer_constants.param.ao_mode == Moer::EAoMode::RTAO_AO_ONLY || moer_constants.param.ao_mode == Moer::EAoMode::SSAO_AO_ONLY ||
        moer_constants.param.ao_mode == Moer::EAoMode::SSDO_AO_ONLY) {
        rw_output[pixel_pos] = float4(lighting.aaa, 1.0);
    } else {
        float3 color         = TextureHandle(moer_constants.param.color_tex).SampleLevel<float3>(uv);
        rw_output[pixel_pos] = float4(color * lighting.a + lighting.rgb, 1.0);
    }
}
