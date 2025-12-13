#ifndef MOER_MATERIALS_PBR_HLSLI
#define MOER_MATERIALS_PBR_HLSLI

#include "shared/raster/ShaderParameters.h"
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
 * References:
 * - https://github.com/QianMo/PBR-White-Paper
 * - GAMES202 Homework4 Code
 * 如果你对学习PBR/BRDF感兴趣，推荐阅读毛星云大神的文章和GAMES202的课程作业代码，链接如上
 */

struct BRDFContext {
    // MARK: Vars

    float3 light_radiance;
    float  roughness;
    float3 albedo;
    float  metallic;
    float  alpha;
    float  alpha2;

    float3 N;
    float3 V;
    float3 L;
    float3 H;

    float NoV;
    float NoL;
    float VoH;
    float NoH;
    float NoH2;

    float3 kulla_conty_E_o;
    float3 kulla_conty_E_i;
    float3 kulla_conty_E_avg;
    // kulla_conty_E_i   = TextureHandle(lut_ggx_emu_handle).Sample2D<float3>(float2(NoL, roughness));
    // kulla_conty_E_o   = TextureHandle(lut_ggx_emu_handle).Sample2D<float3>(float2(NoV, roughness));
    // kulla_conty_E_avg = TextureHandle(lut_ggx_eavg_handle).Sample2D<float3>(float2(0.0, roughness));

    // Config
    uint  BRDF_multi_scatter; // kulla-conty approximation
    uint  NDF_mode;           // EBrdfNdfMode: Beckmann, GGX, GTR2, GTR1
    uint  G_mode;             // 用 Vis_SmithJointGGX 来代替 G_Smith
    uint  G_is_ibl;           // 是否使用IBL的Fresnel近似

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

        alpha  = roughness * roughness;
        alpha2 = alpha * alpha;

        N = _N;
        V = _V;

        // saturate比max(x,0)更快
        NoV = saturate(dot(N, V));

        kulla_conty_E_i   = _kulla_conty_E_i;
        kulla_conty_E_avg = _kulla_conty_E_avg;
    }

    void SetConfig(
        uint  _BRDF_multi_scatter,
        uint  _NDF_mode,
        uint  _G_mode,
        uint  _G_is_ibl
    ) {
        BRDF_multi_scatter = _BRDF_multi_scatter;
        NDF_mode           = _NDF_mode;
        G_mode             = _G_mode;
        G_is_ibl           = _G_is_ibl;
    }

    void UpdatePerLight(float3 _light_radiance, float3 _L, float3 _kulla_conty_E_o) {
        light_radiance = _light_radiance;

        L = _L;
        H = normalize(V + _L);

        // saturate比max(x,0)更快
        NoL = saturate(dot(N, L));
        VoH = saturate(dot(V, H));
        NoH = saturate(dot(N, H));
        NoH2 = NoH * NoH;

        kulla_conty_E_o = _kulla_conty_E_o;
    }

    // MARK: NDF

    float _D_Beckmann() {
        // 这里实现过程中有一个坑，如果NoH==0，那么 NoH2 -> 0
        // 这会出现一个 0/0 型的 NaN
        // 这个bug的表现就是一些物体的边缘会非常黑

        float NoH2_Safe = max(NoH2, Epsilon); // 0/0型nan

        // 可以取消注释下面这行，试一下不考虑0/0时的效果，很有趣（是在某种特风格游戏里常见到的渲染效果！？）
        // NoH2_Safe = NoH2; 

        return exp((NoH2_Safe - 1.0) / (alpha2 * NoH2_Safe)) / (PI * alpha2 * NoH2_Safe * NoH2_Safe);
    }

    float _D_GGX() {
        float den = (NoH2 * (alpha2 - 1.0) + 1.0);
        return alpha2 / (PI * den * den);
    }

    float _D_GTR2() {    // 完全等价于GGX
        return _D_GGX(); // ShaderLanguageCompiler的优化策略是极其激进的内联展开，不用担心这里的开销
    }

    float _D_GTR1() {
        // 这里实现过程中有一个坑，如果roughness==1，那么 log(alpha2) -> 0 且 (alpha2 - 1.0) -> 0
        // 这会出现一个 0/0 型的 NaN
        // 这个bug的表现就是一些物体会全黑

        float a2 = clamp(alpha2, Epsilon, 1.0 - Epsilon);

        float den = (1.0 + (a2 - 1.0) * NoH2);
        return (a2 - 1.0) / (PI * den * log(a2));
    }

    // MARK: Geometry
    // Geometry Functions (Shadowing-Masking)
    // - 注：理论上，Geometry函数和NDF应该是配套使用的。换句话说，替换了NDF，那么对应的Geometry函数也应该更换
    // - 此处简化实现，假设所有Geometry函数均基于GGX NDF
    // - （即 如果你将NDF切换为Beckmann，那么实际上渲染结果是非物理正确的）

    float _G_SchlickApprox_G1(float NoX) { // (UE4) Schlick Approximation
        // 直接光: k = (r + 1)^2 / 8
        // IBL:   k = (r^2) / 2
        float a = roughness;
        float k = (G_is_ibl) ? (a * a) / 2.0 : ((a + 1) * (a + 1)) / 8.0;

        return NoX / (NoX * (1.0 - k) + k);
    }

    float _G_SchlickApprox_G2() { // (UE4) Schlick Approximation
        float ggx1 = _G_SchlickApprox_G1(NoV);
        float ggx2 = _G_SchlickApprox_G1(NoL);
        return ggx1 * ggx2;
    }

    // Note: Vis = G / (4 * NoL * NoV)

    float _Vis_SmithJointApprox_UE4() {
        float Vis_SmithV = NoL * (NoV * (1.0 - alpha) + alpha);
        float Vis_SmithL = NoV * (NoL * (1.0 - alpha) + alpha);
        return 0.5 / (Vis_SmithV + Vis_SmithL);
    }

    float _Vis_SmithJointApprox_Unity() {
        float lambdaV = NoL * NoV * (1.0 - roughness) + roughness;
        float lambdaL = NoV * sqrt((-NoL * alpha + NoL) * NoL + alpha);
        return 0.5 / (lambdaV + lambdaL);
    }

    float _Vis_SmithJointApprox_Filament() {
        float ggxV = NoL * sqrt((-NoV * alpha2 + NoV) * NoV + alpha2);
        float ggxL = NoV * sqrt((-NoL * alpha2 + NoL) * NoL + alpha2);
        return 0.5 / (ggxV + ggxL);
    }

    float _Vis_SmithJointApprox_RespawnEntertainment() {
        float x = 2.0 * NoL * NoV;
        float y = NoL + NoV;
        return 0.5 / lerp(x, y, alpha);
    }

    // MARK: Fresnel
    // Schlick's Approximation
    // 注意，这里参数是VoH，而不是NoV。使用半程向量计算才是对的，使用Normal计算是错的！
    float3 _F_SchlickApprox(float3 F0) {
        return F0 + (1.0 - F0) * Math::pow5(1.0 - VoH);
    }

    // MARK: BRDF

    float3 _BRDF_Microfacet(float3 F0) {
        // Fresnel
        float3 F = _F_SchlickApprox(F0);

        // NDF
        // - 为什么GTR1和GTR2不合并？因为GTX(Extending GGX)没有 *形状不变性 shape-invariant*，所以无法用一个统一的公式计算
        // - 这也是为什么GTR没有被广泛使用的原因之一
        float D = (NDF_mode == Moer::EBrdfNdfMode::BECKMANN) ? _D_Beckmann() :
                  (NDF_mode == Moer::EBrdfNdfMode::GGX) ? _D_GGX() :
                  (NDF_mode == Moer::EBrdfNdfMode::GTR2) ? _D_GTR2() :
                  (NDF_mode == Moer::EBrdfNdfMode::GTR1) ? _D_GTR1() : _D_GGX();

        // Geometry (Shadowing-Masking)
        float Vis = 1.0;

        if (G_mode == Moer::EBrdfGMode::G_SCHLICK) {
            // Separable Smith-GGX

            float G = _G_SchlickApprox_G2();

            Vis = G / max(Epsilon, 4.0 * NoL * NoV);

        } else {
            // Joint Smith-GGX

            Vis = (G_mode == Moer::EBrdfGMode::VIS_UE4) ? _Vis_SmithJointApprox_UE4() :
                  (G_mode == Moer::EBrdfGMode::VIS_UNITY) ? _Vis_SmithJointApprox_Unity() :
                  (G_mode == Moer::EBrdfGMode::VIS_FILAMENT) ? _Vis_SmithJointApprox_Filament() :
                  (G_mode == Moer::EBrdfGMode::VIS_RESPAWN) ? _Vis_SmithJointApprox_RespawnEntertainment() :
                                                            _Vis_SmithJointApprox_UE4();
        }

        return D * F * Vis;
    }

    // MARK: Multi-Scatter BRDF
    // Kulla-Conty (From GAMES202 Homework4)

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

    // MARK: Evaluate
    float3 Evaluate() {
        float3 F0 = float3(0.04, 0.04, 0.04);
        F0        = lerp(albedo, F0, metallic); // 在games202hw4代码框架里，这个地方写反了

        float3 brdf_microfacet   = _BRDF_Microfacet(F0);
        float3 brdf_multi_scatter = BRDF_multi_scatter ? _BRDF_MultiScatter() : float3(0.0, 0.0, 0.0);

        float3 brdf = brdf_microfacet + brdf_multi_scatter;

        float3 color = brdf * light_radiance * NoL;

        return color;
    }
};

#endif