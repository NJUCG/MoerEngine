#ifndef SHADOW_DEPTH_PASS
#define SHADOW_DEPTH_PASS 0
#endif

#include "core/common/Bindless.hlsl"
#include "core/common/Common.hlsl"
BINDLESS_BINDINGS(3, 2, 4, 5)
#include "shared/Geometry.h"
#include "shared/raster/ShaderParameters.h"
#include "shared/scene/SharedSceneStruct.h"

#include "materials/Material.hlsli"
#include "pipelines/raster/deferred/geometry/GeometryPassCommon.hlsli"

[[vk::push_constant]] ConstantBuffer<Moer::GeometryPassBindlessParam> param;

void DiscardByAlphaTest(uint material_id, float2 uv_in_map) {
    if (param.enable_alpha_test == 0) {
        return;
    }
    
    ArrayBuffer material_buf = ArrayBuffer(param.material_buf_hdl);

    Moer::GMaterial mat = material_buf.Load<Moer::GMaterial>(material_id);

    // 是否需要discard该像素（AlphaMode为BLEND或MASK时，需要进行discard）
    if (mat.alpha_mode == Moer::EAlphaMode::Mask) {
        float alpha = TextureHandle(mat.albedo_map_hdl).Sample2D<float4>(uv_in_map).a * mat.albedo_factor.a;
        // printf("MASK: alpha: %f, cutoff: %f, albedo_map.a: %f, base_color.a: %f\n", alpha, mat.alpha_cutoff, TextureHandle(mat.albedo_map).Sample2D<float4>(uv_in_map).a, mat.base_color_factor.a);
        if (alpha < mat.alpha_cutoff) {
            discard;
        }
    } else if (mat.alpha_mode == Moer::EAlphaMode::Blend) {
        float alpha = TextureHandle(mat.albedo_map_hdl).Sample2D<float4>(uv_in_map).a * mat.albedo_factor.a;
        // printf("BLEND: alpha: %f; albedo_map.a: %f; base_color.a: %f\n", alpha, TextureHandle(mat.albedo_map).Sample2D<float4>(uv_in_map).a, mat.base_color_factor.a);
        if (alpha < param.alpha_test_blend_pixel_cutoff) {
            discard;
        }
    }
}

#if SHADOW_DEPTH_PASS // MARK: ShadowDepthPass

void main(VsOutput input) : SV_TARGET {

    DiscardByAlphaTest(input.material_id, input.texcoord0); // 此处有可能触发discard，直接终止shader
}

#else // MARK: GeometryPass

struct PsOutput {
    float4 base_color : SV_TARGET0;
    float4 normal : SV_TARGET1;
    float4 metal_rough_ao : SV_TARGET2;
};

// Wang hash + HSV 三维散列：将整数 ID 映射为视觉上高对比度且唯一性强的 RGB 颜色
// 使用 H/S/V 三个维度独立变化，比仅变化色相的方案区分度高数十倍
float3 ClusterIdToColor(uint id) {
    uint h = id;
    h = (h ^ 61u) ^ (h >> 16u);
    h = h * 9u;
    h = h ^ (h >> 4u);
    h = h * 0x27d4eb2du;
    h = h ^ (h >> 15u);

    float hue = frac(float(h & 0xFFFFu) / 65535.0);
    float sat = 0.55 + 0.45 * frac(float((h >> 16u) & 0xFFu) / 255.0); // [0.55, 1.0]
    float val = 0.65 + 0.35 * frac(float((h >> 24u) & 0xFFu) / 255.0); // [0.65, 1.0]

    float3 rgb = saturate(abs(frac(hue + float3(0.0, 2.0/3.0, 1.0/3.0)) * 6.0 - 3.0) - 1.0);
    return lerp(float3(1,1,1), rgb, sat) * val;
}

PsOutput main(VsOutput input) : SV_TARGET {

    DiscardByAlphaTest(input.material_id, input.texcoord0); // 此处有可能触发discard，直接终止shader

    // Debug 可视化模式（debug_visualization_mode: 0=off, 1=Cluster ID, 2=frac(UV), 3=顶点法线）
    if (param.debug_visualization_mode > 0) {
        PsOutput output;
        output.metal_rough_ao = float4(0.0, 0.5, 1.0, 0.0);

        if (param.debug_visualization_mode == 1) {
            output.base_color = float4(ClusterIdToColor(input.primitive_id), 1.0);
            output.normal = float4(Raster::PackNormal(normalize(input.normal)), 1.0);
        } else if (param.debug_visualization_mode == 2) {
            output.base_color = float4(frac(input.texcoord0), 0.0, 1.0);
            output.normal = float4(Raster::PackNormal(normalize(input.normal)), 1.0);
        } else {
            float3 n = normalize(input.normal);
            output.base_color = float4(n * 0.5 + 0.5, 1.0);
            output.normal = float4(Raster::PackNormal(n), 1.0);
        }
        return output;
    }

    ArrayBuffer material_buf = ArrayBuffer(param.material_buf_hdl);
    Moer::GMaterial mat = material_buf.Load<Moer::GMaterial>(input.material_id);

    float3 base_color = SampleTextureAndApplyFactor(
        mat.albedo_map_hdl,
        input.texcoord0,
        mat.albedo_factor.xyz,
        MISSING_TEXTURE_COLOR
    );
    float2 metallic_roughness = SampleTextureAndApplyFactor(
        mat.metallic_roughness_map_hdl,
        input.texcoord0,
        float2(mat.metallic_factor, mat.roughness_factor),
        float2(mat.metallic_factor, mat.roughness_factor)
    );
    float material_ao = SampleTextureAndApplyFactor(mat.ao_map_hdl, input.texcoord0, 1.0, 1.0);
    float3 shading_normal = GetNormalFromNormalMap(
        mat.normal_map_hdl,
        input.texcoord0,
        normalize(input.normal),
        normalize(input.tangent)
    );

    PsOutput output;
    output.base_color = float4(base_color, 0.0);
    output.normal = float4(Raster::PackNormal(shading_normal), 1.0);
    output.metal_rough_ao = float4(metallic_roughness, material_ao, 0.0);

    return output;
}

#endif // SHADOW_DEPTH_PASS