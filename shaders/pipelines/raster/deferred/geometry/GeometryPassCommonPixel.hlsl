#ifndef SHADOW_DEPTH_PASS
#define SHADOW_DEPTH_PASS 0
#endif

#include "core/common/Bindless.hlsl"
#include "core/common/Common.hlsl"
BINDLESS_BINDINGS(3, 2, 4, 5)
#include "shared/Geometry.h"
#include "shared/raster/ShaderParameters.h"

#include "materials/Material.hlsli"
#include "pipelines/raster/deferred/geometry/VertexFactory.hlsl"

[[vk::push_constant]] ConstantBuffer<Moer::GeometryPassBindlessParam> param;

void DiscardByAlphaTest(uint mat_idx_and_type, float2 uv_in_map) {
    if (param.enable_alpha_test == 0) {
        return;
    }

    // 是否需要discard该像素（AlphaMode为BLEND或MASK时，需要进行discard）
    uint mat_type, mat_id;
    GetMaterialTypeAndIndex(mat_idx_and_type, mat_type, mat_id);
    MaterialData mat = UnpackMaterialData<MaterialData>(param.material_buffer, mat_id);

    if (mat.alpha_mode == Moer::EAlphaMode::AM_MASK) {
        float alpha = TextureHandle(mat.albedo_map).Sample2D<float4>(uv_in_map).a * mat.base_color_factor.a;
        // printf("MASK: alpha: %f, cutoff: %f, albedo_map.a: %f, base_color.a: %f\n", alpha, mat.alpha_cutoff, TextureHandle(mat.albedo_map).Sample2D<float4>(uv_in_map).a, mat.base_color_factor.a);
        if (alpha < mat.alpha_cutoff) {
            discard;
        }
    } else if (mat.alpha_mode == Moer::EAlphaMode::AM_BLEND) {
        float alpha = TextureHandle(mat.albedo_map).Sample2D<float4>(uv_in_map).a * mat.base_color_factor.a;
        // printf("BLEND: alpha: %f; albedo_map.a: %f; base_color.a: %f\n", alpha, TextureHandle(mat.albedo_map).Sample2D<float4>(uv_in_map).a, mat.base_color_factor.a);
        if (alpha < param.alpha_test_blend_pixel_cutoff) {
            discard;
        }
    }
}

#if SHADOW_DEPTH_PASS // MARK: ShadowDepthPass

void main(VertexFactory::VsOutput input) : SV_TARGET {
    ArrayBuffer geometry_data_array = ArrayBuffer(param.geometry_data);

    Moer::GeometryData geom_data = Moer::LoadGeometryData(
        geometry_data_array.GetByteAddressBuffer(), input.instance_id * sizeof(Moer::GeometryData)
    );

    DiscardByAlphaTest(geom_data.mat_idx_and_type, input.texcoord0); // 此处有可能触发discard，直接终止shader
}

#else // MARK: GeometryPass

struct PsOutput {
    uint   vbuffer : SV_TARGET0;
    float4 normal : SV_TARGET1;
    float4 tangent : SV_TARGET2;
    float2 texcoord0 : SV_TARGET3;
    float4 position : SV_TARGET4;
};

PsOutput main(VertexFactory::VsOutput input) : SV_TARGET {
    ArrayBuffer geometry_data_array = ArrayBuffer(param.geometry_data);

    Moer::GeometryData geom_data = Moer::LoadGeometryData(
        geometry_data_array.GetByteAddressBuffer(), input.instance_id * sizeof(Moer::GeometryData)
    );

    DiscardByAlphaTest(geom_data.mat_idx_and_type, input.texcoord0); // 此处有可能触发discard，直接终止shader
    

    PsOutput output;
    output.vbuffer   = geom_data.mat_idx_and_type;
    output.normal    = float4(Raster::PackNormal(normalize(input.normal)), 1.0);
    output.tangent   = float4(Raster::PackNormal(normalize(input.tangent)), 1.0);
    output.texcoord0 = input.texcoord0;
    output.position  = float4(input.world_position, 1.0);

    return output;
}

#endif // SHADOW_DEPTH_PASS