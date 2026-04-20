#include "core/common/Bindless.hlsl"
#include "core/common/Common.hlsl"
BINDLESS_BINDINGS(3, 2, 4, 5)
#include "shared/ShaderParameters.h"
#include "shared/raster/ShaderParameters.h"

[[vk::push_constant]] ConstantBuffer<Moer::AoCompositeParam> param;
[[vk::binding(0, 0)]] RWTexture2D<float4>                    rw_output;

// Joint Bilateral Upsample：用全分辨率深度+法线引导，对半分辨率AO进行边缘保持上采样
float JointBilateralUpsampleAO(float2 uv) {
    float2 ao_res     = param.ao_resolution;
    float2 inv_ao_res = 1.0 / ao_res;

    // 全分辨率像素处的深度和法线
    float  depth_center  = TextureHandle(param.depth_tex).SampleLevel<float>(uv);
    float3 normal_center = Raster::UnpackNormal(TextureHandle(param.normal_tex).SampleLevel<float3>(uv));

    // 低分辨率纹理空间中的亚像素位置
    float2 low_pos    = uv * ao_res - 0.5;
    int2   base_texel = int2(floor(low_pos));
    float2 frac_pos   = frac(low_pos);

    static const float kDepthScale  = 100.0; // 深度差异灵敏度（相对深度）
    static const float kNormalPower = 32.0;  // 法线差异灵敏度

    float ao_sum = 0.0;
    float w_sum  = 0.0;

    [unroll]
    for (int y = 0; y <= 1; y++) {
        [unroll]
        for (int x = 0; x <= 1; x++) {
            int2   tap    = base_texel + int2(x, y);
            float2 tap_uv = (float2(tap) + 0.5) * inv_ao_res;

            float ao_sample = TextureHandle(param.ao_tex).SampleLevel<float>(tap_uv);

            // 在低分辨率像素中心位置采样全分辨率深度和法线（用于边缘判断）
            float  depth_sample  = TextureHandle(param.depth_tex).SampleLevel<float>(tap_uv);
            float3 normal_sample = Raster::UnpackNormal(TextureHandle(param.normal_tex).SampleLevel<float3>(tap_uv));

            // 空间权重（双线性插值权重）
            float w_spatial = (x == 0 ? (1.0 - frac_pos.x) : frac_pos.x)
                            * (y == 0 ? (1.0 - frac_pos.y) : frac_pos.y);

            // 深度权重：相对深度差越大，权重越低
            float depth_diff = abs(depth_center - depth_sample) / max(depth_center, 1e-6);
            float w_depth    = exp(-depth_diff * kDepthScale);

            // 法线权重：法线夹角越大，权重越低
            float w_normal = pow(max(dot(normal_center, normal_sample), 0.0), kNormalPower);

            float w = w_spatial * w_depth * w_normal;
            ao_sum += ao_sample * w;
            w_sum  += w;
        }
    }

    // 所有权重都很小时回退到双线性采样（天空等无几何区域）
    return w_sum > 1e-6 ? ao_sum / w_sum : TextureHandle(param.ao_tex).SampleLevel<float>(uv);
}

[numthreads(8, 8, 1)] void main(uint2 pixel_pos : SV_DispatchThreadID) {
    if (pixel_pos.x >= uint(param.full_resolution.x) || pixel_pos.y >= uint(param.full_resolution.y))
        return;

    float2 uv = (float2(pixel_pos) + 0.5) * param.inv_full_resolution;

    float ao;
    if (param.is_half_resolution) {
        ao = JointBilateralUpsampleAO(uv);
    } else {
        ao = TextureHandle(param.ao_tex).SampleLevel<float>(uv);
    }

    if (param.ao_mode == Moer::EAoMode::RTAO_AO_ONLY || param.ao_mode == Moer::EAoMode::SSAO_AO_ONLY ||
        param.ao_mode == Moer::EAoMode::SSDO_AO_ONLY) {
        rw_output[pixel_pos] = float4(ao, ao, ao, 1.0);
    } else {
        float3 color         = TextureHandle(param.color_tex).SampleLevel<float3>(uv);
        rw_output[pixel_pos] = float4(color * ao, 1.0);
    }
}
