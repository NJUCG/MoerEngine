#ifndef MOER_MATERIALS_PBR_HLSLI
#define MOER_MATERIALS_PBR_HLSLI

#include "core/math/Math.hlsli" // 假设这里有 PI 的定义

static const float3 Fdielectric = 0.04;
static const float  Epsilon     = 0.0001;

// --- BRDF Functions ---
float ndfGGX(float cosLh, float roughness) {
    float alpha   = roughness * roughness;
    float alphaSq = alpha * alpha;
    float denom   = (cosLh * cosLh) * (alphaSq - 1.0) + 1.0;
    return alphaSq / (PI * denom * denom);
}

float gaSchlickG1(float cosTheta, float k) {
    return cosTheta / (cosTheta * (1.0 - k) + k);
}

float gaSchlickGGX(float cosLi, float cosLo, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return gaSchlickG1(cosLi, k) * gaSchlickG1(cosLo, k);
}

float3 fresnelSchlick(float3 F0, float cosTheta) {
    return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
}

// --- PBR Info Struct ---
struct PBRInfo {
    float  roughness;
    float3 albedo;
    float  metalness;
    float3 normal;
    float3 viewDir;

    float3 Evaluate(float3 lightDir) {
        float3 F0           = lerp(Fdielectric, albedo, metalness);
        float3 halfDir      = normalize(lightDir + viewDir);
        float  cosLi        = saturate(dot(normal, lightDir));
        float  cosLh        = saturate(dot(normal, halfDir));
        float  cosLo        = saturate(dot(normal, viewDir));

        float3 F            = fresnelSchlick(F0, cosLo);
        float  D            = ndfGGX(cosLh, roughness);
        float  G            = gaSchlickGGX(cosLi, cosLo, roughness);

        float3 kd           = lerp(float3(1, 1, 1) - F, float3(0, 0, 0), metalness);
        float3 diffuseBRDF  = kd * albedo;
        float3 specularBRDF = (F * D * G) / max(Epsilon, 4.0 * cosLi * cosLo);

        return (diffuseBRDF + specularBRDF) * cosLi;
    }
};

#endif