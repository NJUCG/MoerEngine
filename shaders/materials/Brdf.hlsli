#ifndef MOER_MATERIALS_PBR_HLSLI
#define MOER_MATERIALS_PBR_HLSLI

#include "core/math/Math.hlsli"

/**
 * PBR BRDF Functions
 * 
 * N: Normal, 法线方向
 * L: Light Direction, 光照方向
 * V: View Direction, 视线方向
 * H: Halfway Vector, 半程向量 (normalized(L + V))
 * 
 * NoL: dot(N, L)
 * NoV: dot(N, V)
 * etc..
 *
 * D: NDF, Normal Distribution Function, 法线分布函数
 * F: Fresnel Term, 菲涅尔项
 * G: Geometry Term (Shadowing-Masking Term), 几何遮蔽项 
 *
 * color = BRDF * Radiance * NoL(cosTheta)
 * 
 * - BRDF = MicrofacetBRDF + MultiScatterBRDF(Kulla-Conty)
 *   我们使用微表面模型，同时用Kulla-Conty方法来近似多次散射，从而达到一个相对物理正确的PBR渲染
 * 
 * - MicrofacetBRDF = (D * F * G) / (4 * NoL * NoV) = D * F * VisTerm
 *   我们支持多种不同D、F、G的计算方式，可以在GUI参数中进行设置
 * 
 * References: GAMES202 Homework4 Code
 */

struct BRDFContext {
    // MARK: Vars

    float3 light_radiance;
    float  roughness;
    float3 albedo;
    float  metallic;

    float3 N;
    float3 V;
    float3 L;
    float3 H;

    float NoV;
    float NoL;
    float VoH;
    float NoH;

    float3 kulla_conty_E_o;
    float3 kulla_conty_E_i;
    float3 kulla_conty_E_avg;
    // kulla_conty_E_i   = TextureHandle(lut_ggx_emu_handle).Sample2D<float3>(float2(NoL, roughness));
    // kulla_conty_E_o   = TextureHandle(lut_ggx_emu_handle).Sample2D<float3>(float2(NoV, roughness));
    // kulla_conty_E_avg = TextureHandle(lut_ggx_eavg_handle).Sample2D<float3>(float2(0.0, roughness));

    // settings
    bool is_ibl;

    // MARK: Init Func

    void Init(
        float  _roughness,
        float3 _albedo,
        float  _metallic,
        float3 _N,
        float3 _V,
        float3 _kulla_conty_E_i,
        float3 _kulla_conty_E_avg
    ) {
        roughness = _roughness;
        albedo    = _albedo;
        metallic  = _metallic;

        N = _N;
        V = _V;

        // saturate比max(x,0)更快
        NoV = saturate(dot(N, V));

        kulla_conty_E_i   = _kulla_conty_E_i;
        kulla_conty_E_avg = _kulla_conty_E_avg;

        is_ibl = true;
    }

    void UpdatePerLight(float3 _light_radiance, float3 _L, float3 _kulla_conty_E_o) {
        light_radiance = _light_radiance;

        L = _L;
        H = normalize(V + _L);

        // saturate比max(x,0)更快
        NoL = saturate(dot(N, L));
        VoH = saturate(dot(V, H));
        NoH = saturate(dot(N, H));

        kulla_conty_E_o = _kulla_conty_E_o;
    }

    // MARK: Microfacet BRDF

    float _D_GGX() {
        float a    = roughness * roughness;
        float a2   = a * a;
        float NoH2 = NoH * NoH;

        float nom   = a2;
        float denom = (NoH2 * (a2 - 1.0) + 1.0);
        denom       = PI * denom * denom;

        return nom / denom;
    }

    float _G_SchlickGGX(float NoX) {
        // 直接光: k = (r + 1)^2 / 8
        // IBL:   k = (r^2) / 2
        float a = roughness;
        float k = (is_ibl) ? (a * a) / 2.0 : ((a + 1) * (a + 1)) / 8.0;

        float nom   = NoX;
        float denom = NoX * (1.0 - k) + k;

        return nom / denom;
    }

    // Geometry
    float _G_Smith() {
        float NoV = max(dot(N, V), 0.0);
        float NoL = max(dot(N, L), 0.0);

        float ggx1 = _G_SchlickGGX(NoV);
        float ggx2 = _G_SchlickGGX(NoL);

        return ggx1 * ggx2;
    }

    // Fresnel, Schlick's Approximation
    // 注意，这里参数是VoH，而不是NoV。使用半程向量计算才是对的，使用Normal计算是错的！
    float3 _F_Schlick(float3 F0) {
        return F0 + (1.0 - F0) * Math::pow5(1.0 - VoH);
    }

    float3 _BRDF_Microfacet(float3 F0) {
        float3 F = _F_Schlick(F0);
        float  D = _D_GGX();
        float  G = _G_Smith();

        float3 nom   = D * F * G;
        float  denom = max(Epsilon, 4.0 * NoL * NoV);

        return nom / denom;
    }

    // MARK: Kulla-Conty Multi-Scatter BRDF (From GAMES202 Homework4)

    // https://blog.selfshadow.com/publications/s2017-shading-course/imageworks/s2017_pbs_imageworks_slides_v2.pdf
    float3 _AverageFresnel(float3 r, float3 g) {
        return 0.087237 + 0.0230685 * g - 0.0864902 * g * g + 0.0774594 * g * g * g + 0.782654 * r -
               0.136432 * r * r + 0.278708 * r * r * r + 0.19744 * g * r + 0.0360605 * g * g * r -
               0.2586 * g * r * r;
    }

    float3 _BRDF_MultiScatter() {
        float3 edgetint = float3(0.827, 0.792, 0.678);
        float3 F_avg    = _AverageFresnel(albedo, edgetint);

        float3 f_ms  = (1.0 - kulla_conty_E_o) * (1.0 - kulla_conty_E_i) / (PI * (1.0 - kulla_conty_E_avg));
        float3 f_add = F_avg * kulla_conty_E_avg / (1.0 - F_avg * (1.0 - kulla_conty_E_avg));

        return f_ms * f_add;
    }

    // MARK: Main Evaluate Func
    float3 Evaluate() {
        float3 F0 = float3(0.04, 0.04, 0.04);
        F0        = lerp(F0, albedo, 1.0 - metallic);

        float3 brdf_microfacet   = _BRDF_Microfacet(F0);
        float3 brdf_multiScatter = _BRDF_MultiScatter();

        float3 brdf = brdf_microfacet + brdf_multiScatter;

        float3 color = brdf * light_radiance * NoL;

        return color;
    }
};

#endif