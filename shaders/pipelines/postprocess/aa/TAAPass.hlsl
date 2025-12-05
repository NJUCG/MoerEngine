// #include <core/math/STL.hlsli>
#include <shared/postprocess/ShaderParameters.h>

[[vk::binding(0, 0)]] ConstantBuffer<Moer::TAAParams> params : register(b0);

#ifndef SAMPLE_COUNT
#define SAMPLE_COUNT 1
#endif

#if SAMPLE_COUNT == 1
[[vk::binding(1, 0)]] Texture2D<float4> unfiltered_rt : register(t0);
[[vk::binding(2, 0)]] Texture2D<float2> motion : register(t1);
#else
[[vk::binding(1, 0)]] Texture2DMS<float4> unfiltered_rt : register(t0);
[[vk::binding(2, 0)]] Texture2DMS<float2> motion : register(t1);
#endif

[[vk::binding(3, 0)]] Texture2D<float4> feedback_rt : register(t2);
[[vk::binding(4, 0)]] SamplerState s_sampler : register(s0);

[[vk::binding(5, 0)]] RWTexture2D<float4> output_rt : register(u0);
[[vk::binding(6, 0)]] RWTexture2D<float4> rw_feedback_rt : register(u1);

#define GROUP_X 16
#define GROUP_Y 16
#define BUFFER_X (GROUP_X + 3)
#define BUFFER_Y (GROUP_Y + 3)
#define RENAMED_GROUP_Y ((GROUP_X * GROUP_Y) / BUFFER_X)

groupshared float4 s_colors_length[BUFFER_Y][BUFFER_X];
groupshared float2 s_motion[BUFFER_Y][BUFFER_X];

// https://en.wikipedia.org/wiki/Perceptual_quantizer
 static const float pq_m1 = 0.1593017578125;
 static const float pq_m2 = 78.84375;
 static const float pq_c1 = 0.8359375;
 static const float pq_c2 = 18.8515625;
 static const float pq_c3 = 18.6875;

float3 PQDecode(float3 _img) {
  float3 Np = pow(max(_img, 0), 1.f / pq_m2);
  float3 l = Np - pq_c1;
  l = l / (pq_c2 - pq_c3 * Np);
  l = pow(max(l, 0.f), 1.f / pq_m1);
  return l * params.pqc;
}

float3 PQEncode(float3 _img) {
  float3 l = _img * params.inv_pqc;
  float3 lm = pow(max(l, 0.f), pq_m1);
  float3 n = (lm * pq_c2 + pq_c1) / (1.f + pq_c3 * lm);
  return saturate(pow(n, pq_m2));
}

// float3 PQDecode(float3 image)
// {
//     float3 Np = pow(max(image, 0.0), 1.0 / pq_m2);
//     float3 L = Np - pq_c1;
//     L = L / (pq_c2 - pq_c3 * Np);
//     L = pow(max(L, 0.0), 1.0 / pq_m1);

//     return L * params.pqc; // returns cd/m^2
// }

// float3 PQEncode(float3 image)
// {
//     float3 L = image * params.inv_pqc;
//     float3 Lm = pow(max(L, 0.0), pq_m1);
//     float3 N = (pq_c1 + pq_c2 * Lm) / (1.0 + pq_c3 * Lm);
//     image = pow(N, pq_m2);

//     return saturate(image);
// }


float3 BicubicSampleCatmullRom(Texture2D _tex, SamplerState _spl, float2 _pos,
                               float2 _inv_tex_size) {
  float2 tc = floor(_pos - 0.5f) + 0.5f;
  float2 f = saturate(_pos - tc);
  float2 f2 = f * f;
  float2 f3 = f2 * f;

  float2 w0 = -0.5f * f3 + f2 - 0.5f * f;
  float2 w1 = 1.5f * f3 - 2.5f * f2 + 1.0f;
  float2 w3 = 0.5f * (f3 - f2);
  float2 w2 = 1.0f - w0 - w1 - w3;
  float2 w12 = w1 + w2;

  float2 tc0 = (tc - 1.f) * _inv_tex_size;
  float2 tc12 = (tc + w2 / w12) * _inv_tex_size;
  float2 tc3 = (tc + 2.f) * _inv_tex_size;

  float3 result =
      _tex.SampleLevel(_spl, float2(tc0.x, tc0.y), 0).rgb * (w0.x * w0.y) +
      _tex.SampleLevel(_spl, float2(tc0.x, tc12.y), 0).rgb * (w0.x * w12.y) +
      _tex.SampleLevel(_spl, float2(tc0.x, tc3.y), 0).rgb * (w0.x * w3.y) +
      _tex.SampleLevel(_spl, float2(tc12.x, tc0.y), 0).rgb * (w12.x * w0.y) +
      _tex.SampleLevel(_spl, float2(tc12.x, tc12.y), 0).rgb * (w12.x * w12.y) +
      _tex.SampleLevel(_spl, float2(tc12.x, tc3.y), 0).rgb * (w12.x * w3.y) +
      _tex.SampleLevel(_spl, float2(tc3.x, tc0.y), 0).rgb * (w3.x * w0.y) +
      _tex.SampleLevel(_spl, float2(tc3.x, tc12.y), 0).rgb * (w3.x * w12.y) +
      _tex.SampleLevel(_spl, float2(tc3.x, tc3.y), 0).rgb * (w3.x * w3.y);

  return max(0, result);
}

void Preload(uint2 _shared_id, int2 _gid) {
#if SAMPLE_COUNT == 1
  float3 color = PQEncode(unfiltered_rt[_gid.xy].rgb);
  float2 m = motion[_gid.xy].xy;
  float motion_length = dot(m, m);
#else

  float3 color = float3(0);
  float motion_length = 0;
  float2 m = float2(0);

  [unroll] for (uint i = 0; i < SAMPLE_COUNT; i++) {
    float3 c = PQEncode(unfiltered_rt.Load(int3(_gid.xy, i)).rgb);
    float2 mo = motion.Load(int3(_gid.xy, i)).xy;
    float ml = dot(mo, mo);

    color += c;
    if (ml > motion_length) {
      motion_length = ml;
      m = mo;
    }
  }
  color /= SAMPLE_COUNT;
#endif

  s_colors_length[_shared_id.y][_shared_id.x] = float4(color, motion_length);
  s_motion[_shared_id.y][_shared_id.x] = m;
}

float2 OutputToInput(int2 _pixel_pos_to_origin) {
  return (float2(_pixel_pos_to_origin) + 0.5f) * params.input_over_output_size -
         0.5f + params.in_view_origin + params.in_pixel_offset;
}

[numthreads(GROUP_X, GROUP_Y, 1)] void main(uint2 dtid
                                            : SV_DispatchThreadID,
                                              uint2 group_id
                                            : SV_GroupID, uint2 gtid
                                            : SV_GroupThreadID) {
  int2 new_id;
  float linear_idx = gtid.y * GROUP_X + gtid.x;
  linear_idx = (linear_idx + 0.5f) / BUFFER_X;

  new_id.y = int(floor(linear_idx));
  new_id.x = int(floor(frac(linear_idx) * BUFFER_X));

  int2 group_base = int2(OutputToInput(group_id * int2(GROUP_X, GROUP_Y)) - 1);

  if (new_id.y < RENAMED_GROUP_Y) {
    Preload(new_id, group_base + new_id);
  }
  new_id.y += RENAMED_GROUP_Y;
  if(new_id.y < BUFFER_Y){
    Preload(new_id, group_base + new_id);
  }

  GroupMemoryBarrierWithGroupSync();

  int2 out_pixel_pos = dtid + int2(params.out_view_origin);
  float2 in_pos = OutputToInput(dtid);
  int2 in_pos_int = int2(round(in_pos));
  int2 in_pos_shared = in_pos_int - group_base - 1;

  float3 color_moment1 = 0;
  float3 color_moment2 = 0;

  float longest_motion = 0;
  int2 longest_motion_pos = 0;
  float3 this_pixel_color = 0;

  [unroll] for (int dy = 0; dy <= 2; ++dy) {
    [unroll] for (int dx = 0; dx <= 2; ++dx) {

        int2 pos = in_pos_shared + int2(dx, dy);
      float4 color_length =
          s_colors_length[pos.y][pos.x];

      float3 color = color_length.rgb;
      float motion_length = color_length.w;

      if (dx == 1 && dy == 1) {
        this_pixel_color = color;
      }

      color_moment1 += color;
      color_moment2 += color * color;

      if (motion_length > longest_motion) {
        longest_motion = motion_length;
        longest_motion_pos = pos;
      }
    }
  }

  float2 longest_mv = s_motion[longest_motion_pos.y][longest_motion_pos.x];
  color_moment1 /= 9.f;
  color_moment2 /= 9.f;

  float3 color_variance = color_moment2 - color_moment1 * color_moment1;
  float3 color_stddev = sqrt(max(color_variance, 0)) * params.clamping_factor;
  float3 color_min = color_moment1 - color_stddev;
  float3 color_max = color_moment1 + color_stddev;

  longest_mv *= params.output_over_input_size;
  float2 src_pos = float2(out_pixel_pos) + longest_mv + 0.5f;

  float3 result_pq;
  if (params.new_frame_weight < 1.f && all(src_pos > params.out_view_origin) &&
      all(src_pos < params.out_view_origin + params.out_view_size)) {
    // float3 history = BicubicSampleCatmullRom(feedback_rt, s_sampler, src_pos,
    //                                          params.out_texture_size_inv);
    float3 history = feedback_rt.SampleLevel(s_sampler, src_pos * params.out_texture_size_inv, 0).rgb;
    float3 history_clamped = history;
    if (params.clamping_factor >= 0) {
      history_clamped = min(color_max, max(color_min, history));
    }

    float motion_weight = smoothstep(0, 1, length(longest_mv));
    float2 dist_to_low_res_pixel = in_pos - float2(in_pos_int);
    float upscaling_factor = params.output_over_input_size.x;
    float sample_weight =
        saturate(1.f - upscaling_factor *
                           dot(dist_to_low_res_pixel, dist_to_low_res_pixel));
    float blend_weight =
        saturate(max(motion_weight, sample_weight) * params.new_frame_weight);

    result_pq = lerp(history_clamped, this_pixel_color, blend_weight);
  } else {
    result_pq = this_pixel_color;
  }
    // result_pq = PQEncode(unfiltered_rt[dtid.xy].rgb);

  float3 result = PQDecode(result_pq);
  output_rt[out_pixel_pos] = float4(result, 1.f);
  rw_feedback_rt[out_pixel_pos] = float4(result_pq, 0.f);
}