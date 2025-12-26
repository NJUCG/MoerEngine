#include "core/common/Bindless.hlsl"
BINDLESS_BINDINGS(3, 2, 4, 5)

#include "core/common/Common.hlsl"
#include "materials/Brdf.hlsli"
#include "materials/Material.hlsli"
#include "pipelines/raster/deferred/lighting/IBL.hlsli"
#include "pipelines/raster/deferred/lighting/Lighting.hlsli"
#include "pipelines/raster/deferred/lighting/shadows/Shadows.hlsli"

#include "shared/raster/ShaderParameters.h"

[[vk::push_constant]] ConstantBuffer<Moer::MaterialPassBindlessParam> param;

float3 WorldPosFromDepth(float depth, float2 screen_uv, float4x4 inv_view_proj) {
    float4 clip    = float4(screen_uv.x * 2.f - 1.f, 1.f - screen_uv.y * 2.f, depth, 1.0);
    float4 world_w = mul(inv_view_proj, clip);
    float3 pos     = world_w.xyz / world_w.w;
    return pos;
}

float4 main(float2 in_uv : TEXCOORD0) : SV_TARGET {
    // MARK: Textures
    uint gbuffer_mat = TextureHandle(param.vbuffer).Sample2D<uint>(in_uv);
    uint mat_type    = gbuffer_mat & 0x000000FF;
    uint mat_id      = (gbuffer_mat & 0xFFFFFF00) >> 8;
    if (mat_type != param.material_type) {
        discard;
    }
    MaterialData mat = UnpackMaterialData<MaterialData>(param.material_buffer, mat_id);

    // MARK: Lighting Data
    ArrayBuffer        global_params = ArrayBuffer(param.global_param_handle);
    Moer::LightingData lighting_data = global_params.Load<Moer::LightingData>(0);

    // MARK: GBuffer
    float2 uv       = TextureHandle(param.gbuffer_uv).Sample2D<float2>(in_uv);
    float  depth    = TextureHandle(param.gbuffer_depth).Sample2D<float>(in_uv);
    float3 normal   = Raster::UnpackNormal(TextureHandle(param.gbuffer_normal).Sample2D<float3>(in_uv));
    float3 tangent  = Raster::UnpackNormal(TextureHandle(param.gbuffer_tangent).Sample2D<float3>(in_uv));
    float3 position = WorldPosFromDepth(depth, in_uv, lighting_data.inv_view_proj);
    // Shoude be reconstructed from depth
    // Old code: float3 position = TextureHandle(param.gbuffer_position).Sample2D<float3>(in_uv);

    // MARK: Skybox
    if (depth == 0.0) {
        float3 pos_inf = WorldPosFromDepth(0.99, in_uv, lighting_data.inv_view_proj);
        // printf("pos_inf: %f, %f, %f\n", pos_inf.x, pos_inf.y, pos_inf.z);
        return calculate_ibl(lighting_data, pos_inf, param.cubemap_handle);
        //return float4(0.0, 0.0, 0.0, 1.0); // Black Skybox
    }

    // MARK: PBR
    float3 albedo =
        GetTextureData<float3>(mat.albedo_map, uv, mat.base_color_factor.xyz, MISSING_TEXTURE_COLOR);

    float2 metallic_roughness = GetTextureData<float2>(
        mat.metallic_roughness_map,
        uv,
        float2(mat.metallic_factor, mat.roughness_factor),
        float2(mat.metallic_factor, mat.roughness_factor)
    );
    float metallic  = metallic_roughness.x;
    float roughness = metallic_roughness.y;

    float3 N   = GetNormalFromNormalMap(mat.normal_map, uv, normal, tangent);
    float3 V   = normalize(lighting_data.camera_position - position.xyz);
    float  NoV = saturate(dot(N, V));

    BRDFContext brdf_ctx;
    brdf_ctx.Init(
        roughness,
        albedo,
        metallic,
        N,
        V,
        TextureHandle(lighting_data.lut_ggx_emu_handle).Sample2D<float3>(float2(NoV, roughness)),
        TextureHandle(lighting_data.lut_ggx_eavg_handle).Sample2D<float3>(float2(0.0, roughness))
    );

    brdf_ctx.SetConfig(
        lighting_data.brdf_enable_multi_scatter,
        lighting_data.brdf_NDF_mode,
        lighting_data.brdf_G_mode,
        lighting_data.brdf_G_is_ibl
    );

    // - Lights
    ArrayBuffer light_buffer = ArrayBuffer(param.light_buffer);

    // - Shadow
    float shadow = calculate_shadow(lighting_data, position, in_uv, normal);

    // MARK: Shading
    LightContext light_ctx;
    light_ctx.Init(brdf_ctx, position, lighting_data.lut_ggx_emu_handle);

    for (uint i = 0; i < lighting_data.light_count; i++) {
        LightData light = light_buffer.Load<LightData>(i);

        light_ctx.AccumulateLight(light, shadow);
    }

    float3 color = light_ctx.GetResult();

    if (param.enable_extra_ambient) {
        color += param.extra_ambient_intensity * param.extra_ambient_color * brdf_ctx.albedo;
    }

    return float4(color, 1.0);
}