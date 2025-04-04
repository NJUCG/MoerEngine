// bind bindless
#include <framework/Bindless.hlsl>
#include <framework/Common.hlsl>

BINDLESS_BINDINGS(3, 2, 4, 5);

#include <framework/Material.hlsl>

#include <shared/Geometry.h>
#include <shared/ShaderParameters.h>
#include <shared/utils/MoerMath.hlsli>
#include <shared/utils/Packing.h>

#include <MathLib/STL.hlsli>
#include <hwrt/GBufferUtils.hlsli>

#define WITH_NRD 1
#ifdef WITH_NRD
#define NRD_HEADER_ONLY
#include <nrd/NRD.hlsli>

#endif

[[vk::binding(0, 0)]] ConstantBuffer<Moer::CompositingConstants> params
    : register(b0);

[[vk::binding(1, 0)]] RWTexture2D<float4> out_color : register(u0, space0);
[[vk::binding(2, 0)]] RWTexture2D<float4> out_motion : register(u1, space0);

[[vk::binding(3, 0)]] Texture2D<float> gbuffer_view_depth
    : register(t0, space0);
[[vk::binding(4, 0)]] Texture2D<uint> gbuffer_diffuse_albedo
    : register(t1, space0);
[[vk::binding(5, 0)]] Texture2D<uint> gbuffer_specular_roughness
    : register(t2, space0);
[[vk::binding(6, 0)]] Texture2D<uint> gbuffer_normal : register(t3, space0);
[[vk::binding(7, 0)]] Texture2D<float4> gbuffer_emissive : register(t4, space0);

[[vk::binding(8, 0)]] Texture2D<float4> diffuse_lighting : register(t5, space0);
[[vk::binding(9, 0)]] Texture2D<float4> specular_lighting
    : register(t6, space0);
[[vk::binding(10, 0)]] Texture2D<float4> denoised_diffuse_lighting
    : register(t7, space0);
[[vk::binding(11, 0)]] Texture2D<float4> denoised_specular_lighting
    : register(t8, space0);

[numthreads(8, 8, 1)] void main(uint2 gtid
                                : SV_DISPATCHTHREADID) {
  float3 composited_color = float3(0, 0, 0);
  float view_z = gbuffer_view_depth[gtid];

  if (view_z != FP16_MAX) {
    float3 normal = Math::OctToNdirUnorm32(gbuffer_normal[gtid]);
    float3 diffuse_albedo =
        Moer::Unpack_R11G11B10_UFLOAT(gbuffer_diffuse_albedo[gtid]);
    float3 specular_rf0 =
        Moer::Unpack_R8G8B8A8_Gamma_UFLOAT(gbuffer_specular_roughness[gtid])
            .rgb;
    float3 emissive = gbuffer_emissive[gtid].xyz;

    float4 diffuse = diffuse_lighting[gtid];
    float4 specular = specular_lighting[gtid];

#ifdef WITH_NRD
    if (params.denoiser_mode != Moer::s_denoiser_mode_off) {
      float4 denoised_diffuse = denoised_diffuse_lighting[gtid];
      float4 denoised_specular = denoised_specular_lighting[gtid];

      if (params.denoiser_mode == Moer::s_denoiser_mode_reblur) {
        denoised_diffuse =
            REBLUR_BackEnd_UnpackRadianceAndNormHitDist(denoised_diffuse);
        denoised_specular =
            REBLUR_BackEnd_UnpackRadianceAndNormHitDist(denoised_specular);
      }

      // enable mix in debug view later
      diffuse = denoised_diffuse;
      specular = denoised_specular;
    }
#endif
    diffuse.rgb *= diffuse_albedo;
    specular.rgb *= max(specular_rf0, 0.001f);

    composited_color += diffuse.rgb + specular.rgb;
    composited_color += emissive;

  } else {
    // sky
    RayDesc primary_ray = Moer::SetupPrimaryRay(gtid, params.main_view);
    if (params.enable_env_map) {
      TextureHandle env_handle = (TextureHandle)params.env_map_handle;
      float2 uv = Math::DirToEquirectangularUV(primary_ray.Direction);
      uv.x -= params.env_rotation;
      composited_color = env_handle.SampleLevel<float3>(uv, 0);

      //     uint tex_handle =
      //     g__array_114514_bdls[NonUniformResourceIndex(env_handle.handle)];
      // uint tex_idx = tex_handle >> 8;
      // uint sampler_idx = tex_handle & 0xff;
      // printf("sampler_idx %d\n", tex_handle);

      composited_color *= params.env_scale;
    }
    float2 env_motion = Moer::GetEnvMotion(params.main_view, params.prev_view,
                                           float2(gtid) + 0.5f);
    out_motion[gtid] = float4(env_motion, 0.f, 0.f);
  }

  if (any(isnan(composited_color))) {
    composited_color = float3(0, 0, 1);
  }

  out_color[gtid] = float4(composited_color, 1.f);
}