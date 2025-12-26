// Reference: 隔壁Raytracing的Histogram.hlsl
#include "core/math/STL.hlsli"
#include "shared/raster/ShaderParameters.h"

[[vk::push_constant]] ConstantBuffer<Moer::TonemappingPipelineBindlessParam> param;

[[vk::binding(0, 0)]] Texture2D      input_image;
[[vk::binding(1, 0)]] RWBuffer<uint> histogram;

groupshared uint s_histogram[Moer::TONEMAPPING_HISTOGRAM_BIN_COUNT];

[numthreads(Moer::TONEMAPPING_HISTOGRAM_GROUP_X, Moer::TONEMAPPING_HISTOGRAM_GROUP_Y, 1)] void
main(uint gid : SV_GroupIndex, uint2 thread_id : SV_DispatchThreadId) {
    uint2  pos      = thread_id.xy;
    float2 uv       = (float2(pos) + 0.5) * param.resolution_inv; // (0, 1)
    bool   is_valid = all(pos < param.resolution);                // all(v) <=> (v.x && v.y)

    if (gid < Moer::TONEMAPPING_HISTOGRAM_BIN_COUNT) {
        s_histogram[gid] = 0;
    }

    GroupMemoryBarrierWithGroupSync();

    if (is_valid) {
        float3 color     = input_image[pos].rgb;
        float  luminance = STL::Color::Luminance(color);
        float  log2lum = saturate(log2(luminance) * param.ae.log2lum_to01_scale + param.ae.log2lum_to01_bias);
        float  histogram_bin = log2lum * (Moer::TONEMAPPING_HISTOGRAM_BIN_COUNT - 1);

        uint left_bin  = uint(floor(histogram_bin));
        uint right_bin = left_bin + 1;

        // 这里相当于总计给直方图贡献1.0的权重，通过将float向上放大为整数来避免精度问题
        // => left + right == 1.0
        uint left_weight  = uint((frac(histogram_bin)) * Moer::TONEMAPPING_HISTOGRAM_POINT_FRAC_MULTIPLIER);
        uint right_weight = Moer::TONEMAPPING_HISTOGRAM_POINT_FRAC_MULTIPLIER - left_weight;

        if (left_weight != 0 && left_bin < Moer::TONEMAPPING_HISTOGRAM_BIN_COUNT) {
            InterlockedAdd(s_histogram[left_bin], left_weight);
        }

        if (right_weight != 0 && right_bin < Moer::TONEMAPPING_HISTOGRAM_BIN_COUNT) {
            InterlockedAdd(s_histogram[right_bin], right_weight);
        }
    }

    GroupMemoryBarrierWithGroupSync();

    if (gid < Moer::TONEMAPPING_HISTOGRAM_BIN_COUNT) {
        uint local_bin_val = s_histogram[gid];
        if (local_bin_val != 0) {
            InterlockedAdd(histogram[gid], local_bin_val);
        }
    }
}