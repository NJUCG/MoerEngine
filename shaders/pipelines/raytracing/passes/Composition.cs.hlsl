#include <framework/Bindless.hlsl>
#include <framework/Common.hlsl>
BINDLESS_BINDINGS(2, 1, 3, 4);
#include <framework/Lighting.hlsl>

#include <framework/Material.hlsl>

#include <nrd/NRD.hlsli>

struct Param {
    float4x4 view2world;
    float4 frustum;
    float3 dir;
    float orthomode;
    uint2 rect;
    float2 inv_rect;
    float2 jitter;
    uint denoiser_type;
};

[[vk::push_constant]] ConstantBuffer<Param> param;
[[vk::binding(0, 0)]] Texture2D<float4> in_normal_roughness: register(t0, space0);
[[vk::binding(1, 0)]] Texture2D<float4> in_basecolor_metalness: register(t1, space0);
[[vk::binding(2, 0)]] Texture2D<float> in_view_z : register(t2, space0);
[[vk::binding(3, 0)]] Texture2D<float3> in_mv : register(t3, space0);
[[vk::binding(4, 0)]] Texture2D<float4> in_shadow;
[[vk::binding(5, 0)]] Texture2D<float4> in_diffuse;
[[vk::binding(6, 0)]] Texture2D<float4> in_specular;
[[vk::binding(7, 0)]] Texture2D<float3> in_direct_lighting;
[[vk::binding(8, 0)]] Texture2D<float3> in_emission;

[[vk::binding(9, 0)]] RWTexture2D<float3> out_composed_diff : register(u0, space0);
[[vk::binding(10, 0)]] RWTexture2D<float3> out_composed_spec : register(u1, space0);

[numthreads(16, 16, 1)] void main(uint2 pixel_pos : SV_DispatchThreadID) {
    if (any(pixel_pos >= param.rect)) {
        return;
    }
    float2 pixel_uv = float2(pixel_pos + 0.5) * param.inv_rect;
    float2 sample_uv = pixel_uv + param.jitter;

    // ViewZ
    float view_z = in_view_z[pixel_pos];
    float3 lemi = in_emission[pixel_pos];

    // Normal, roughness and material ID
    float material_id;
    float4 normal_roughness = NRD_FrontEnd_UnpackNormalAndRoughness(in_normal_roughness[pixel_pos], material_id);
    float3 normal = normal_roughness.xyz;
    float roughness = normal_roughness.w;

    // Early out - sky
    if(abs(view_z) >= INF) {
        out_composed_diff[pixel_pos] = lemi;
        out_composed_spec[pixel_pos] = float3(0, 0, 0);
        return;
    }

    // Direct sun lighting * shadow + emission
    float4 shadow_data = in_shadow[pixel_pos];

    // #if( SIGMA_TRANSLUCENT == 1 )
    float3 shadow = SIGMA_BackEnd_UnpackShadow(shadow_data).yzw;
    // #else
    // float shadow = SIGMA_BackEnd_UnpackShadow(shadow_data).x;
    // #endif

    float3 l_direct = in_direct_lighting[pixel_pos] * shadow + lemi;

    // G-buffer
    float3 albedo, Rf0;
    float4 basecolor_metalness = in_basecolor_metalness[pixel_pos];
    STL::BRDF::ConvertBaseColorMetalnessToAlbedoRf0(basecolor_metalness.xyz, basecolor_metalness.w, albedo, Rf0);

    float3 x_view = STL::Geometry::ReconstructViewPosition(sample_uv, param.frustum, view_z, param.orthomode);
    float3 x = mul(param.view2world, float4(x_view, 1.f)).xyz;
    // float3 view = param.orthomode == 0 ? normalize(STL::Geometry::RotateVector(param.view2world, -x_view)) : param.dir.xyz;
    float3 view = param.orthomode == 0 ? normalize(mul((float3x3)param.view2world, -x_view)) : param.dir.xyz;

    // Sample NRD outputs
    float4 diff = in_diffuse[pixel_pos];
    float4 spec = in_specular[pixel_pos];

    // Decode NORMAL mode outputs
    #if 1
        if(param.denoiser_type == DENOISER_RELAX){
            diff = RELAX_BackEnd_UnpackRadiance(diff);
            spec = RELAX_BackEnd_UnpackRadiance(spec);
        } else {
            diff = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(diff);
            spec = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(spec);
        }
    #endif

    // ( Optional ) RELAX doesn't support AO / SO
    if (param.denoiser_type == DENOISER_RELAX) {
        diff.w = 1.0 / Math::Pi( 1.0 );
        spec.w = 1.0 / Math::Pi( 1.0 );
    }

    // Material modulation ( convert radiance back into irradiance )
    float NoV = abs(dot(normal, view));
    float3 Fenv = STL::BRDF::EnvironmentTerm_Rtg(Rf0, NoV, roughness);
    float3 diff_demod = (1.0 - Fenv) * albedo * 0.99 + 0.01;
    float3 spec_demod = Fenv * 0.99 + 0.01;

    // Composition
    float3 l_diff = diff.xyz * diff_demod;
    float3 l_spec = spec.xyz * spec_demod;

    // IMPORTANT: we store diffuse and specular separately to be able to use the reprojection trick. Let's assume that direct lighting can always be reprojected as diffuse
    l_diff += l_direct;

    // Output
    out_composed_diff[pixel_pos] = l_diff + l_spec;
    out_composed_spec[pixel_pos] = l_spec;
}