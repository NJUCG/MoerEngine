#ifndef SHADOW_DEPTH_PASS
#define SHADOW_DEPTH_PASS 0
#endif

#include "core/common/Bindless.hlsl"
#include "core/common/Common.hlsl"
BINDLESS_BINDINGS(3)
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
        float alpha = mat.albedo_map_hdl > 0
                        ? TextureHandle(mat.albedo_map_hdl).Sample2D<float4>(uv_in_map).a * mat.albedo_factor.a
                        : mat.albedo_factor.a;
        // printf("MASK: alpha: %f, cutoff: %f, albedo_map.a: %f, base_color.a: %f\n", alpha, mat.alpha_cutoff, TextureHandle(mat.albedo_map).Sample2D<float4>(uv_in_map).a, mat.base_color_factor.a);
        if (alpha < mat.alpha_cutoff) {
            discard;
        }
    } else if (mat.alpha_mode == Moer::EAlphaMode::Blend) {
        float alpha = mat.albedo_map_hdl > 0
                        ? TextureHandle(mat.albedo_map_hdl).Sample2D<float4>(uv_in_map).a * mat.albedo_factor.a
                        : mat.albedo_factor.a;
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

PsOutput main(VsOutput input) : SV_TARGET {

    DiscardByAlphaTest(input.material_id, input.texcoord0); // 此处有可能触发discard，直接终止shader

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